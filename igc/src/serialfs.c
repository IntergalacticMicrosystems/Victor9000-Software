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
#include "dosapi.h"
#include "keyboard.h"

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

/*---------------------------------------------------------------------------
 * Directory-tree transfer (recursive copy/delete of whole directories)
 *
 * Local walks use the DOS find_first/find_next cursor held in a per-level stack
 * DTA, exactly like fops_copy_dir. Remote walks can't recurse mid-listing -
 * ftx_list_dir streams entries through a callback and a new request can't be
 * issued until it finishes - so each level's entries are first stashed in a
 * far-heap "walk arena" used as a LIFO across the recursion, then processed.
 *---------------------------------------------------------------------------*/

#define SFS_TREE_MAX_DEPTH 24       /* mirrors fileops.c MAX_DIR_DEPTH */

/* Compact arena record: only what the recursion needs to decide and name. */
typedef struct {
    char    name[13];
    uint8_t attr;
} sfs_walk_ent;

static sfs_walk_ent __far *g_walk = (sfs_walk_ent __far *)0;
static uint16_t g_walk_cap = 0;     /* arena capacity, in entries        */
static uint16_t g_walk_used = 0;    /* entries currently live (all levels) */
static uint8_t  g_tree_depth = 0;   /* recursion guard for tree ops       */

/* ESC between files aborts a tree op (each file already has its own bar). */
static bool_t sfs_tree_cancelled(void)
{
    if (kbd_check()) {
        KeyEvent k = kbd_get();
        if (k.type == KEY_ASCII && k.code == KEY_ESC) {
            return TRUE;
        }
    }
    return FALSE;
}

/* One allocation serves a whole (possibly nested) remote walk; freed at the end.
 * 8 KB (~585 entries summed across the active path) comfortably exceeds any
 * realistic Victor tree; on overflow the op aborts cleanly. */
static bool_t sfs_walk_alloc(void)
{
    g_walk_cap = (uint16_t)(8192 / sizeof(sfs_walk_ent));
    g_walk = (sfs_walk_ent __far *)mem_alloc((uint32_t)g_walk_cap * sizeof(sfs_walk_ent));
    if (g_walk == (sfs_walk_ent __far *)0) {
        g_walk_cap = (uint16_t)(1024 / sizeof(sfs_walk_ent));
        g_walk = (sfs_walk_ent __far *)mem_alloc((uint32_t)g_walk_cap * sizeof(sfs_walk_ent));
    }
    g_walk_used = 0;
    g_tree_depth = 0;
    return (g_walk != (sfs_walk_ent __far *)0) ? TRUE : FALSE;
}

static void sfs_walk_free(void)
{
    if (g_walk != (sfs_walk_ent __far *)0) {
        mem_free(g_walk);
        g_walk = (sfs_walk_ent __far *)0;
    }
    g_walk_cap = 0;
    g_walk_used = 0;
}

/* ftx_list_dir callback: append each real entry (skip "." / "..") to the arena.
 * On overflow it flags the caller but keeps returning 0 so ftx_list_dir drains
 * the remaining response pages - stopping early would desync the packet stream. */
static int sfs_walk_cb(const ftx_dir_entry_t *e, void *user_data)
{
    bool_t *overflow = (bool_t *)user_data;

    if (e->name[0] == '.' &&
        (e->name[1] == '\0' || (e->name[1] == '.' && e->name[2] == '\0'))) {
        return 0;
    }
    if (g_walk_used >= g_walk_cap) {
        *overflow = TRUE;               /* keep draining; just stop storing */
        return 0;
    }
    str_copy_n(g_walk[g_walk_used].name, e->name, 13);
    g_walk[g_walk_used].attr = e->attr;
    g_walk_used++;
    return 0;
}

/*---------------------------------------------------------------------------
 * Download a remote tree (server -> local). Recurses depth-first.
 *---------------------------------------------------------------------------*/
static int sfs_get_tree_r(const char *remote_rel, const char *local_path)
{
    char rchild[MAX_FULL_PATH];
    char lchild[MAX_FULL_PATH];
    uint16_t base, end, i;
    bool_t overflow = FALSE;
    int result = 0;

    if (g_tree_depth >= SFS_TREE_MAX_DEPTH) {
        ui_error("Directory tree too deep");
        kbd_wait();
        return -1;
    }

    /* Create the local destination directory. */
    if (dos_mkdir(local_path) != 0 && !dos_exists(local_path)) {
        ui_error("Cannot create directory");
        kbd_wait();
        return -1;
    }

    /* Collect this directory's entries into the arena above the current mark. */
    base = g_walk_used;
    if (ftx_list_dir(&g_ftx, remote_rel, sfs_walk_cb, &overflow) < 0) {
        ui_error("Serial server not responding");
        kbd_wait();
        g_walk_used = base;
        return -1;
    }
    if (overflow) {
        ui_error("Directory too large to copy");
        kbd_wait();
        g_walk_used = base;
        return -1;
    }
    end = g_walk_used;

    g_tree_depth++;
    for (i = base; i < end && result == 0; i++) {
        char    name[13];
        uint8_t attr;

        str_copy_n(name, g_walk[i].name, sizeof(name));
        attr = g_walk[i].attr;

        str_copy(rchild, remote_rel);
        path_append(rchild, name);
        str_copy(lchild, local_path);
        path_append(lchild, name);

        if (attr & DOS_ATTR_DIRECTORY) {
            if (sfs_get_tree_r(rchild, lchild) != 0) {
                result = -1;
            }
        } else if (serialfs_get_file(rchild, lchild) != 0) {
            result = -1;
        }

        if (result == 0 && sfs_tree_cancelled()) {
            result = -1;
        }
    }
    g_tree_depth--;

    g_walk_used = base;                 /* pop this level's entries */
    return result;
}

/*---------------------------------------------------------------------------
 * Upload a local tree (local -> server). The local side is walked with the DOS
 * find cursor (per-level DTA, like fops_copy_dir); files are pushed with a
 * path-qualified name so the server stores them under the right subdirectory.
 *---------------------------------------------------------------------------*/
static int sfs_put_tree_r(const char *local_path, const char *remote_rel)
{
    char src_child[MAX_FULL_PATH];
    char rchild[MAX_FULL_PATH];
    char pattern[MAX_FULL_PATH];
    DTA dta;
    DTA __far *old_dta;
    int result = 0;

    if (g_tree_depth >= SFS_TREE_MAX_DEPTH) {
        ui_error("Directory tree too deep");
        kbd_wait();
        return -1;
    }

    /* Create the remote directory. Best effort: it may already exist, and file
       uploads auto-create parents server-side; this also covers empty dirs. */
    serialfs_mkdir(remote_rel);

    str_copy(pattern, local_path);
    path_append(pattern, "*.*");

    g_tree_depth++;
    old_dta = dos_get_dta();
    dos_set_dta(&dta);

    if (dos_find_first(pattern, 0x37) == 0) {
        do {
            if (dta.name[0] == '.' &&
                (dta.name[1] == '\0' ||
                 (dta.name[1] == '.' && dta.name[2] == '\0'))) {
                continue;
            }

            str_copy(src_child, local_path);
            path_append(src_child, dta.name);
            str_copy(rchild, remote_rel);
            path_append(rchild, dta.name);

            if (dta.attr & DOS_ATTR_DIRECTORY) {
                result = sfs_put_tree_r(src_child, rchild);
            } else {
                /* serialfs_put_named lays down the per-file progress bar. */
                result = serialfs_put_named(src_child, rchild);
            }

            if (result == 0 && sfs_tree_cancelled()) {
                result = -1;
            }
        } while (result == 0 && dos_find_next() == 0);
    }

    dos_set_dta(old_dta);
    g_tree_depth--;
    return result;
}

/*---------------------------------------------------------------------------
 * Delete a remote tree (files, then subdirs, then the directory itself).
 *---------------------------------------------------------------------------*/
static int sfs_delete_tree_r(const char *remote_rel)
{
    char rchild[MAX_FULL_PATH];
    uint16_t base, end, i;
    bool_t overflow = FALSE;
    int result = 0;

    if (g_tree_depth >= SFS_TREE_MAX_DEPTH) {
        ui_error("Directory tree too deep");
        kbd_wait();
        return -1;
    }

    base = g_walk_used;
    if (ftx_list_dir(&g_ftx, remote_rel, sfs_walk_cb, &overflow) < 0) {
        ui_error("Serial server not responding");
        kbd_wait();
        g_walk_used = base;
        return -1;
    }
    if (overflow) {
        ui_error("Directory too large to delete");
        kbd_wait();
        g_walk_used = base;
        return -1;
    }
    end = g_walk_used;

    g_tree_depth++;
    for (i = base; i < end && result == 0; i++) {
        char    name[13];
        uint8_t attr;

        str_copy_n(name, g_walk[i].name, sizeof(name));
        attr = g_walk[i].attr;

        str_copy(rchild, remote_rel);
        path_append(rchild, name);

        ui_show_progress("Deleting", name, 0, 1);
        if (attr & DOS_ATTR_DIRECTORY) {
            if (sfs_delete_tree_r(rchild) != 0) {
                result = -1;
            }
        } else if (serialfs_delete(rchild) != 0) {
            result = -1;
        }

        if (result == 0 && sfs_tree_cancelled()) {
            result = -1;
        }
    }
    g_tree_depth--;
    g_walk_used = base;

    /* The directory is now empty; remove it (server rmdir). */
    if (result == 0 && serialfs_delete(remote_rel) != 0) {
        result = -1;
    }
    return result;
}

int serialfs_get_tree(const char *remote_rel, const char *local_path)
{
    int rc;

    if (!g_connected) {
        return -1;
    }
    if (!sfs_walk_alloc()) {
        ui_error("Out of memory for directory copy");
        kbd_wait();
        return -1;
    }
    rc = sfs_get_tree_r(remote_rel, local_path);
    sfs_walk_free();
    return rc;
}

int serialfs_put_tree(const char *local_path, const char *remote_rel)
{
    if (!g_connected) {
        return -1;
    }
    g_tree_depth = 0;
    return sfs_put_tree_r(local_path, remote_rel);
}

int serialfs_delete_tree(const char *remote_rel)
{
    int rc;

    if (!g_connected) {
        return -1;
    }
    if (!sfs_walk_alloc()) {
        ui_error("Out of memory for directory delete");
        kbd_wait();
        return -1;
    }
    rc = sfs_delete_tree_r(remote_rel);
    sfs_walk_free();
    return rc;
}
