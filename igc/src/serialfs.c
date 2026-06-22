/*
 * serialfs.c - igc serial file-server backend implementation
 *
 * Talks to igcfs.py on a modern PC over the Victor's COM1 (Serial A) using the
 * vetted viclibc polled serial + packet + filetrx layers. igc is the client;
 * igcfs is the server.
 */

#include "igc.h"
#include "panel.h"
#include "util.h"
#include "ui.h"
#include "mem.h"

/* The viclibc structs are 1-byte packed on the wire (the library is built
 * -zp1). igc itself uses its default packing, so wrap these includes in
 * pack(1) to keep the shared structs (ftx_state_t, ftx_start_t, dir entries)
 * byte-compatible with the library objects and the Python server. */
#pragma pack(push, 1)
#include "serial.h"
#include "packet.h"
#include "filetrx.h"
#include "ftx_crc32.h"
#pragma pack(pop)

#include "serialfs.h"
#include "serialfs_proto.h"

/*---------------------------------------------------------------------------
 * Single serial session state (only one COM1 link exists)
 *---------------------------------------------------------------------------*/
static pkt_state_t g_pkt;
static ftx_state_t g_ftx;
static bool_t      g_connected = FALSE;

/* Scratch buffer for building/reading short control packets, kept off the
 * 8 KB stack (igc is single-threaded, so a module-static is fine). */
static uint8_t g_sfs_buf[PKT_MAX_PAYLOAD];

/* Disk staging buffer for file transfers. viclibc2 no longer owns this - the
 * app allocates it and registers it with ftx_set_io_buffer. Sized by memory
 * tier (grows when RAM allows; see mem_get_xfer_buf_size), it batches floppy
 * I/O: write-batching on receive (used inside filetrx) and read-ahead on send
 * (used by serialfs_put_named below). Allocated in serialfs_connect, freed in
 * serialfs_disconnect, so it outlives every transfer made on the session. */
static uint8_t __far *g_io_buf = (uint8_t __far *)0;
static uint16_t       g_io_bufsize = 0;

/*---------------------------------------------------------------------------
 * Connection management
 *---------------------------------------------------------------------------*/
bool_t serialfs_connect(uint8_t port, uint8_t baud_idx)
{
    ser_init();                         /* both ports 8N1 */
    ser_set_baud(port, baud_idx);
    pkt_init_polled(&g_pkt, port);      /* polled mode - no ISR on this port */
    ftx_init(&g_ftx, &g_pkt);

    /* Allocate the app-owned staging buffer and hand it to filetrx. Same tiered
     * size + single fallback-to-floor as the editor/copy buffers. Without it,
     * ftx_receive_file/ftx_send_file fail with FTX_ERR_MEMORY. */
    g_io_bufsize = mem_get_xfer_buf_size();
    g_io_buf = (uint8_t __far *)mem_alloc(g_io_bufsize);
    if (g_io_buf == (uint8_t __far *)0) {
        g_io_bufsize = XFER_BUF_TINY;
        g_io_buf = (uint8_t __far *)mem_alloc(g_io_bufsize);
    }
    if (g_io_buf == (uint8_t __far *)0) {
        g_io_bufsize = 0;
        g_connected = FALSE;
        return FALSE;
    }
    ftx_set_io_buffer(&g_ftx, (uint8_t *)g_io_buf, g_io_bufsize);

    /* Resync sequence bits with the server. Fails if nothing answers. */
    if (!pkt_send_reset(&g_pkt)) {
        mem_free(g_io_buf);
        g_io_buf = (uint8_t __far *)0;
        g_io_bufsize = 0;
        g_connected = FALSE;
        return FALSE;
    }

    g_connected = TRUE;
    return TRUE;
}

void serialfs_disconnect(void)
{
    /* Leave the remote server running (it may serve later reconnects); just
     * forget the local session and release the staging buffer. */
    mem_free(g_io_buf);
    g_io_buf = (uint8_t __far *)0;
    g_io_bufsize = 0;
    g_connected = FALSE;
}

bool_t serialfs_is_connected(void)
{
    return g_connected;
}

/*---------------------------------------------------------------------------
 * Directory listing
 *---------------------------------------------------------------------------*/
typedef struct {
    Panel   *p;
    uint16_t count;
} sfs_list_ctx;

/* Called by ftx_list_dir for each remote entry; append it to the file list. */
static int sfs_list_cb(const ftx_dir_entry_t *e, void *user_data)
{
    sfs_list_ctx *ctx = (sfs_list_ctx *)user_data;
    FileEntry __far *fe;

    /* Stop if the panel is full (caller marks the list truncated). */
    if (ctx->count >= ctx->p->files.capacity) {
        return 1;
    }

    /* Skip a stray "." (the server normally omits it; ".." is kept). */
    if (e->name[0] == '.' && e->name[1] == '\0') {
        return 0;
    }

    fe = &ctx->p->files.entries[ctx->count];
    fe->attr = e->attr;
    fe->time = e->time;
    fe->date = e->date;
    fe->size = e->size;
    str_copy_n(fe->name, e->name, 13);
    fe->selected = 0;
    ctx->count++;
    return 0;
}

int serialfs_read_dir(Panel *p)
{
    sfs_list_ctx ctx;
    int rc;

    if (!g_connected) {
        return -1;
    }

    ui_status("Reading server...");

    p->files.count = 0;
    p->files.truncated = FALSE;
    p->sel_count = 0;

    ctx.p = p;
    ctx.count = 0;
    rc = ftx_list_dir(&g_ftx, p->path, sfs_list_cb, &ctx);

    p->files.count = ctx.count;
    if (ctx.count >= p->files.capacity) {
        p->files.truncated = TRUE;
    }

    /* Match the local panel's ordering (".." first, dirs, then alpha). */
    panel_sort(p);

    if (p->cursor >= p->files.count) {
        p->cursor = (p->files.count > 0) ? p->files.count - 1 : 0;
    }
    if (p->top > p->cursor) {
        p->top = p->cursor;
    }

    ui_clear_status();

    if (rc < 0) {
        ui_error("Serial server not responding");
        return -1;
    }
    return 0;
}

/*---------------------------------------------------------------------------
 * File transfer
 *---------------------------------------------------------------------------*/

/* Name shown on the transfer progress bar (set per transfer). */
static char g_xfer_name[14];

/* Lay down the progress frame once at 0%. ui_progress_tick then repaints only
 * the delta per chunk - it must stay tiny because it runs in the gap between
 * packets, where a full redraw would starve the polled receive and overrun the
 * Victor's 3-byte serial FIFO at 38400. */
static void sfs_xfer_begin(const char *path)
{
    str_copy_n(g_xfer_name, path_basename(path), sizeof(g_xfer_name));
    ui_show_progress("Copying", g_xfer_name, 0, 1);
}

/* viclibc progress hook for the receive (download) path. */
static int sfs_progress_cb(ftx_state_t *state, const ftx_stats_t *stats)
{
    (void)state;
    ui_progress_tick(stats->chunks_done, stats->total_chunks);
    return 0;                           /* never abort from the bar */
}

int serialfs_get_file(const char *remote_rel, const char *local_path)
{
    ftx_start_t req;
    uint8_t n;
    int rc;

    if (!g_connected) {
        return -1;
    }

    /* Request packet: a START whose direction asks the server to SEND the
     * named file. We then receive it with the stock receive path. */
    mem_set_far(&req, 0, sizeof(req));
    req.cmd = FTX_CMD_START;
    req.direction = FTX_DIR_PC_TO_VICTOR;   /* data flows PC -> Victor */
    req.compression = FTX_COMP_NONE;
    n = 0;
    while (remote_rel[n] != '\0' && n < FTX_MAX_FILENAME - 1) {
        req.filename[n] = remote_rel[n];
        n++;
    }
    req.filename[n] = '\0';
    req.name_len = n;

    /* Lay down the progress frame BEFORE sending the request: once the request
     * is out the server immediately streams its START reply, and a full redraw
     * in that window would overrun the receive FIFO (the ticks during the
     * transfer are deliberately tiny - see ui_progress_tick). */
    sfs_xfer_begin(remote_rel);

    if (pkt_send(&g_pkt, (const uint8_t *)&req, sizeof(req)) < 0) {
        return -1;
    }

    rc = ftx_receive_file(&g_ftx, local_path, 1, sfs_progress_cb);
    return (rc == FTX_OK) ? 0 : -1;
}

/* Upload local_path, telling the server to store it as remote_name (a bare
 * 8.3 name) in the directory igc last listed (the serial panel's current dir).
 * Mirrors the corrected ftx_send_file flow but lets us name the destination
 * independently of the local (possibly temp) filename. */
static int serialfs_put_named(const char *local_path, const char *remote_name)
{
    ftx_file_t fh;
    ftx_start_t start;
    ftx_data_t __far *dpkt;
    ftx_end_t end;
    uint32_t fsize, fcrc;
    uint16_t total, chunk, fdate, ftime;
    int16_t nread;
    int16_t rlen;
    uint8_t n;

    if (!g_connected) {
        return -1;
    }
    fh = ftx_file_open(local_path, FTX_OPEN_READ);
    if (fh == FTX_FILE_INVALID) {
        return -1;
    }

    /* Reset the bar to 0% up front: the outer copy draws it at 100% (file 1 of
     * 1), and the CRC pre-read below plus the START/READY handshake take a
     * second or two - without this the bar would sit at 100% until the first
     * chunk. */
    sfs_xfer_begin(remote_name);

    fsize = ftx_file_size(fh);

    /* CRC-32 of the whole file (matches the server's verification). Read in big
     * io-buffer blocks rather than 1 KB chunks - fewer, larger floppy reads. */
    fcrc = FTX_CRC32_INIT;
    while ((nread = ftx_file_read(fh, (uint8_t *)g_io_buf, g_io_bufsize)) > 0) {
        fcrc = ftx_crc32_update(fcrc, (uint8_t *)g_io_buf, (uint32_t)nread);
    }
    fcrc ^= FTX_CRC32_INIT;
    ftx_file_get_datetime(fh, &fdate, &ftime);
    ftx_file_seek(fh, 0, FTX_SEEK_SET);

    total = (uint16_t)((fsize + FTX_CHUNK_SIZE - 1) / FTX_CHUNK_SIZE);

    /* START: direction Victor->PC asks the server to store the file. */
    mem_set_far(&start, 0, sizeof(start));
    start.cmd = FTX_CMD_START;
    start.direction = FTX_DIR_VICTOR_TO_PC;
    start.compression = FTX_COMP_NONE;
    start.flags = FTX_FLAG_OVERWRITE;
    start.file_size = fsize;
    start.file_date = fdate;
    start.file_time = ftime;
    start.file_attr = FTX_ATTR_ARCHIVE;
    n = 0;
    while (remote_name[n] != '\0' && n < FTX_MAX_FILENAME - 1) {
        start.filename[n] = remote_name[n];
        n++;
    }
    start.filename[n] = '\0';
    start.name_len = n;
    start.file_crc = fcrc;
    if (pkt_send(&g_pkt, (const uint8_t *)&start, sizeof(start)) < 0) {
        ftx_file_close(fh);
        return -1;
    }

    /* Wait for READY. */
    rlen = pkt_receive(&g_pkt, g_sfs_buf, PKT_MAX_PAYLOAD);
    if (rlen < 2 || g_sfs_buf[0] != FTX_CMD_READY || g_sfs_buf[1] != 0) {
        ftx_file_close(fh);
        return -1;
    }

    /* DATA chunks. Read the file in big io-buffer blocks (fewer floppy reads),
     * then carve FTX_CHUNK_SIZE slices into the DATA packet built in pkt_buf.
     * io_bufsize is a multiple of FTX_CHUNK_SIZE, so only the file's final block
     * yields a short last slice. The bar tick sits after pkt_send (which has
     * already collected this chunk's ACK), so it draws in the lull before the
     * next slice - no inbound bytes are in flight to be missed. */
    dpkt = (ftx_data_t __far *)g_ftx.pkt_buf;
    chunk = 0;
    while (chunk < total) {
        int16_t  blk;
        uint16_t off;

        blk = ftx_file_read(fh, (uint8_t *)g_io_buf, g_io_bufsize);
        if (blk <= 0) {
            ftx_file_close(fh);
            return -1;
        }
        for (off = 0; off < (uint16_t)blk; off += FTX_CHUNK_SIZE) {
            uint16_t slice = (uint16_t)blk - off;
            if (slice > FTX_CHUNK_SIZE) {
                slice = FTX_CHUNK_SIZE;
            }
            dpkt->cmd = FTX_CMD_DATA;
            dpkt->flags = 0;
            dpkt->chunk_num = chunk;
            dpkt->chunk_size = slice;
            mem_copy_far(dpkt->data, g_io_buf + off, slice);
            if (pkt_send(&g_pkt, g_ftx.pkt_buf, (uint16_t)(6 + slice)) < 0) {
                ftx_file_close(fh);
                return -1;
            }
            chunk++;
            ui_progress_tick(chunk, total);
        }
    }
    ftx_file_close(fh);

    /* END. */
    end.cmd = FTX_CMD_END;
    end.total_chunks = total;
    end.bytes_sent = fsize;
    end.file_crc = fcrc;
    end.status = FTX_STATUS_OK;
    if (pkt_send(&g_pkt, (const uint8_t *)&end, sizeof(end)) < 0) {
        return -1;
    }
    return 0;
}

int serialfs_put_file(const char *local_path, const char *remote_rel)
{
    /* Only the final name matters: the server stores it in its current dir
     * (the serial panel's directory), which is where remote_rel points. */
    return serialfs_put_named(local_path, path_basename(remote_rel));
}

/*---------------------------------------------------------------------------
 * Build a server-relative path "DIR\\NAME" (or "NAME" at the root) from a
 * serial panel's current path and a filename.
 *---------------------------------------------------------------------------*/
void serialfs_build_rel(Panel *p, const char *name, char *buf)
{
    const char *path = p->path;

    if (path[0] == '\\') {
        path++;                 /* drop the leading backslash */
    }
    if (path[0] == '\0') {
        str_copy(buf, name);
    } else {
        str_copy(buf, path);
        path_append(buf, name);
    }
}

/*---------------------------------------------------------------------------
 * Directory operations (protocol extension)
 *---------------------------------------------------------------------------*/

/* Append a length-prefixed string to g_sfs_buf at offset; return new offset. */
static uint16_t sfs_put_str(uint16_t off, const char *s)
{
    uint8_t n = 0;
    while (s[n] != '\0' && n < FTX_MAX_FILENAME - 1) {
        g_sfs_buf[off + 1 + n] = (uint8_t)s[n];
        n++;
    }
    g_sfs_buf[off] = n;
    return off + 1 + n;
}

/* Receive the server's reply to a control command; 0 if it was SFS_CMD_OK. */
static int sfs_wait_ok(void)
{
    int16_t len = pkt_receive(&g_pkt, g_sfs_buf, PKT_MAX_PAYLOAD);
    if (len < 1) {
        return -1;
    }
    return (g_sfs_buf[0] == SFS_CMD_OK) ? 0 : -1;
}

static int sfs_path_command(uint8_t cmd, const char *rel)
{
    uint16_t len;

    if (!g_connected) {
        return -1;
    }
    g_sfs_buf[0] = cmd;
    len = sfs_put_str(1, rel);
    if (pkt_send(&g_pkt, g_sfs_buf, len) < 0) {
        return -1;
    }
    return sfs_wait_ok();
}

int serialfs_delete(const char *remote_rel)
{
    return sfs_path_command(SFS_CMD_DELETE, remote_rel);
}

int serialfs_mkdir(const char *remote_rel)
{
    return sfs_path_command(SFS_CMD_MKDIR, remote_rel);
}

int serialfs_rename(const char *old_rel, const char *new_rel)
{
    uint16_t len;

    if (!g_connected) {
        return -1;
    }
    g_sfs_buf[0] = SFS_CMD_RENAME;
    len = sfs_put_str(1, old_rel);
    len = sfs_put_str(len, new_rel);
    if (pkt_send(&g_pkt, g_sfs_buf, len) < 0) {
        return -1;
    }
    return sfs_wait_ok();
}
