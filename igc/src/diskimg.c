/*
 * diskimg.c - Victor floppy <-> disk image file (F6 More)
 *
 * A floppy is moved one whole track at a time: INT 25h/26h on the disk side,
 * one file append / one serial exchange on the image side. The image is in
 * DOS logical-sector order (side 0 tracks 0..79, then side 1 tracks 0..79),
 * i.e. exactly what MAME loads, so logical sector N is byte N*512.
 *
 * .IMG = the raw image (626,688 or 1,224,192 bytes).
 * .RLE = 16-byte header + the raw image RLE-encoded (viclibc ftx_compress
 *        scheme) as one continuous stream:
 *          0  8  magic "V9KRLE" 1Ah 01h (last byte = format version)
 *          8  4  raw image size, little-endian
 *         12  4  CRC-32 of the raw image (ftx_crc32), little-endian
 */

#include "igc.h"
#include "diskimg.h"
#include "dosapi.h"
#include "dialog.h"
#include "panel.h"
#include "ui.h"
#include "util.h"
#include "mem.h"
#include "keyboard.h"
#include "serialfs.h"

#pragma pack(push, 1)
#include "ftx_crc32.h"
#include "ftx_compress.h"
#pragma pack(pop)

/*---------------------------------------------------------------------------
 * Geometry
 *---------------------------------------------------------------------------*/
#define SECTORS_SS     1224u        /* 626,688 bytes   */
#define SECTORS_DS     2391u        /* 1,224,192 bytes */
#define TRK_MAX_BYTES  9728u        /* 19 sectors x 512 */
#define SCRATCH_SIZE   2048u        /* RLE worst case of one 1 KB slice */
#define RLE_SLICE      1024u
#define RLE_HDR_SIZE   16

/* Zoned recording: {sectors per track, tracks in the zone}. Side 0's eight
 * zones, then side 1's; walking them in order also yields the linear track
 * number (side 1 track 0 = linear 80) and the first logical sector. */
static const uint8_t g_zones[16][2] = {
    {19, 4}, {18, 12}, {17, 11}, {16, 11}, {15, 10}, {14, 12}, {13, 11}, {12, 9},
    {18, 8}, {17, 11}, {16, 11}, {15, 10}, {14, 12}, {13, 11}, {12, 12}, {11, 5}
};

/* Sector count of linear track t (0..159); *first = its first logical sector. */
static uint8_t trk_geom(uint8_t t, uint16_t *first)
{
    uint16_t sec = 0;
    uint8_t i;

    for (i = 0; i < 16; i++) {
        if (t < g_zones[i][1]) {
            *first = sec + (uint16_t)t * g_zones[i][0];
            return g_zones[i][0];
        }
        sec += (uint16_t)g_zones[i][0] * g_zones[i][1];
        t -= g_zones[i][1];
    }
    *first = sec;
    return 0;
}

/* Sidedness lives in the disk label: bit 0 of the word at offset 32 of
 * logical sector 0 (the start of track 0's buffer). */
#define LABEL_SECTORS(trk0) (((trk0)[32] & 1) ? SECTORS_DS : SECTORS_SS)

/*---------------------------------------------------------------------------
 * INT 25h / INT 26h (absolute disk read / write)
 *
 * AL = drive (0 = A), CX = sector count, DX = first logical sector, DS:BX =
 * buffer. Unlike int86x-friendly calls these return with RETF, leaving the
 * caller's FLAGS word on the stack, and may clobber every register except the
 * segment registers. So: save BP/SI/DI/DS, point DS at the buffer, and after
 * the INT pop the leftover flags word into a scratch register - POP does not
 * touch FLAGS, so the INT's carry survives (popf/add sp,2 would lose it).
 * The pops restore DS, then SBB DX,DX turns CF into DX = FFFFh (error, AX =
 * DOS code: AL driver status, AH BIOS-style status) or 0 (success).
 *---------------------------------------------------------------------------*/
uint32_t abs_read(uint8_t drive, uint16_t count, uint16_t first, void __far *buf);
#pragma aux abs_read = \
    "push bp"       \
    "push si"       \
    "push di"       \
    "push ds"       \
    "push es"       \
    "pop  ds"       \
    "int  25h"      \
    "pop  di"       \
    "pop  ds"       \
    "pop  di"       \
    "pop  si"       \
    "pop  bp"       \
    "sbb  dx, dx"   \
    parm [al] [cx] [dx] [es bx] \
    value [dx ax]   \
    modify [ax bx cx dx es];

uint32_t abs_write(uint8_t drive, uint16_t count, uint16_t first, void __far *buf);
#pragma aux abs_write = \
    "push bp"       \
    "push si"       \
    "push di"       \
    "push ds"       \
    "push es"       \
    "pop  ds"       \
    "int  26h"      \
    "pop  di"       \
    "pop  ds"       \
    "pop  di"       \
    "pop  si"       \
    "pop  bp"       \
    "sbb  dx, dx"   \
    parm [al] [cx] [dx] [es bx] \
    value [dx ax]   \
    modify [ax bx cx dx es];

static uint8_t __far *g_trk = (uint8_t __far *)0;   /* one track      */
static uint8_t __far *g_scr = (uint8_t __far *)0;   /* RLE scratch    */
static uint16_t g_disk_err;     /* AX of the last failed INT 25h/26h      */
static uint8_t  g_exe_drive = 0xFF;

void diskimg_set_exe_drive(uint8_t drive)
{
    g_exe_drive = drive;
}

/* Move track t between g_trk and the floppy with one INT call. FALSE on
 * error, with the DOS code in g_disk_err (a code of 0 is valid: write-protect).
 * If real DOS turns out to reject a 19-sector call, split it here into one
 * call per sector (CX = 1, DX = first + i, buffer g_trk + i * 512); the
 * track stays the unit everywhere else. */
static bool_t disk_io(bool_t wr, uint8_t drive, uint8_t t)
{
    uint16_t first;
    uint8_t n = trk_geom(t, &first);
    uint32_t r = wr ? abs_write(drive, n, first, g_trk)
                    : abs_read(drive, n, first, g_trk);

    if ((uint16_t)(r >> 16) != 0) {
        g_disk_err = (uint16_t)r;
        return FALSE;
    }
    return TRUE;
}

/* AL is the block driver's status (the INT 24h error codes); AH the
 * BIOS-style status, used only as a fallback. */
static const char *disk_err_msg(uint16_t ax)
{
    static char buf[16];
    static const char hex[] = "0123456789ABCDEF";
    uint8_t i;

    switch ((uint8_t)ax) {
        case 0x00: return "Disk is write-protected";
        case 0x02: return "Drive not ready";
        case 0x04: return "Data (CRC) error";
        case 0x06:
        case 0x08: return "Sector not found";
        case 0x07:
        case 0x0C: return "Unknown media / not formatted";
        case 0x0A: return "Write fault";
        case 0x0B: return "Read fault";
    }
    if ((ax >> 8) == 0x80) {
        return "No response from drive";
    }
    str_copy(buf, "Disk error ");
    for (i = 0; i < 4; i++) {
        buf[11 + i] = hex[(ax >> (12 - 4 * i)) & 0x0F];
    }
    buf[15] = '\0';
    return buf;
}

/* disk_io with a Retry/Abort prompt. FALSE = the user gave up. */
static bool_t disk_track(bool_t wr, uint8_t drive, uint8_t t)
{
    char msg[48];

    while (!disk_io(wr, drive, t)) {
        str_copy(msg, disk_err_msg(g_disk_err));
        str_copy(msg + str_len(msg), " - retry?");
        if (dlg_confirm("Disk error", msg) != DLG_YES) {
            return FALSE;
        }
    }
    return TRUE;
}

/*---------------------------------------------------------------------------
 * Image source / destination: a local file (below) or the serial server.
 * Every call returns NULL on success or a message for the user. open() with
 * create = TRUE takes *total_sectors (known once track 0 has been read);
 * with create = FALSE it reports the image's size. If open() fails nothing
 * needs closing; otherwise close(ok = FALSE) discards partial output.
 *---------------------------------------------------------------------------*/
typedef struct {
    const char *(*open)(bool_t create, const char *name, uint16_t *total_sectors);
    const char *(*put_track)(uint8_t track, const uint8_t __far *buf, uint16_t len);
    const char *(*get_track)(uint8_t track, uint8_t __far *buf, uint16_t len);
    const char *(*close)(bool_t ok, uint32_t crc);
} ImgIO;

/* 0 = neither, 1 = .IMG, 2 = .RLE (case-insensitive) */
static uint8_t img_ext(const char *name)
{
    const char *dot = str_find_last(name, '.');

    if (dot == (const char *)0) return 0;
    if (str_cmp_i(dot, ".IMG") == 0) return 1;
    if (str_cmp_i(dot, ".RLE") == 0) return 2;
    return 0;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const uint8_t g_magic[8] = { 'V', '9', 'K', 'R', 'L', 'E', 0x1A, 0x01 };

static const char *loc_preflight(uint16_t total);

static dos_handle_t g_fh;
static bool_t   g_create;
static bool_t   g_rle;
static char     g_lpath[MAX_FULL_PATH];
static uint32_t g_hdr_crc;      /* .RLE read: CRC from the header           */
static uint16_t g_in_pos;       /* .RLE read: next unread byte in g_scr     */
static uint16_t g_in_len;       /*            bytes of file data in g_scr   */
static uint16_t g_run_left;     /*            run bytes still owed          */
static uint8_t  g_run_byte;     /*            the run's byte value          */

/* Undo a half-done open: close, and delete what create made. */
static const char *loc_fail(const char *msg)
{
    dos_close(g_fh);
    if (g_create) {
        dos_delete(g_lpath);
    }
    return msg;
}

static const char *loc_open(bool_t create, const char *path, uint16_t *total)
{
    uint8_t hdr[RLE_HDR_SIZE];
    uint32_t size;
    uint8_t i;

    str_copy(g_lpath, path);
    g_create = create;
    g_rle = (img_ext(path) == 2) ? TRUE : FALSE;
    g_in_pos = g_in_len = g_run_left = 0;

    if (create) {
        g_fh = dos_create(path, 0);
        if (g_fh < 0) {
            return "Cannot create image file";
        }
        if (g_rle) {
            /* CRC is rewritten by loc_close once all tracks are in. */
            for (i = 0; i < 8; i++) hdr[i] = g_magic[i];
            put_le32(hdr + 8, (uint32_t)*total * 512);
            put_le32(hdr + 12, 0);
            if (dos_write(g_fh, hdr, RLE_HDR_SIZE) != RLE_HDR_SIZE) {
                return loc_fail("Cannot write image file");
            }
        }
        return (const char *)0;
    }

    g_fh = dos_open(path, 0);
    if (g_fh < 0) {
        return "Cannot open image file";
    }
    if (g_rle) {
        if (dos_read(g_fh, hdr, RLE_HDR_SIZE) != RLE_HDR_SIZE) {
            return loc_fail("Not a Victor disk image");
        }
        for (i = 0; i < 8; i++) {
            if (hdr[i] != g_magic[i]) {
                return loc_fail("Not a Victor disk image");
            }
        }
        size = get_le32(hdr + 8);
        g_hdr_crc = get_le32(hdr + 12);
    } else {
        size = dos_file_size(g_fh);
    }
    if (size != (uint32_t)SECTORS_SS * 512 && size != (uint32_t)SECTORS_DS * 512) {
        return loc_fail("Not a Victor disk image");
    }
    *total = (uint16_t)(size / 512);
    if (g_rle) {
        const char *err = loc_preflight(*total);
        if (err != (const char *)0) {
            return loc_fail(err);
        }
    }
    return (const char *)0;
}

/* .RLE: compress 1 KB slices into the scratch and append them. Each slice's
 * encoding ends on a token boundary, so the concatenation is one valid
 * continuous stream. */
static const char *loc_put_track(uint8_t t, const uint8_t __far *buf, uint16_t len)
{
    uint16_t off, n, c;

    (void)t;
    if (!g_rle) {
        return (dos_write(g_fh, buf, len) == (int16_t)len)
               ? (const char *)0 : "Cannot write image file (disk full?)";
    }
    for (off = 0; off < len; off += RLE_SLICE) {
        n = len - off;
        if (n > RLE_SLICE) n = RLE_SLICE;
        c = (uint16_t)rle_compress(buf + off, n, g_scr, SCRATCH_SIZE);
        if (c == 0 || dos_write(g_fh, g_scr, c) != (int16_t)c) {
            return "Cannot write image file (disk full?)";
        }
    }
    return (const char *)0;
}

/* Next byte of the .RLE stream (refilling the scratch), or -1 at EOF. */
static int16_t rle_in(void)
{
    if (g_in_pos >= g_in_len) {
        int16_t r = dos_read(g_fh, g_scr, SCRATCH_SIZE);
        if (r <= 0) {
            return -1;
        }
        g_in_len = (uint16_t)r;
        g_in_pos = 0;
    }
    return g_scr[g_in_pos++];
}

/* Streaming .RLE decoder: emits exactly len bytes per call.
 *
 * Tokens: b (b != 90h) = literal; 90h 00h = literal 90h; 90h c b = c+3
 * copies of b. A token is read only when at least one output byte is still
 * wanted, and every token emits at least one byte, so the only state that
 * crosses a track boundary is a run cut short by the end of the track
 * (g_run_byte + g_run_left), drained first on the next call. A token that
 * straddles a scratch refill is handled inside rle_in, so no "escape
 * pending" state is needed. Thus a file compressed in one go (runs crossing
 * track and sector boundaries) decodes the same as one written per slice. */
static const char *loc_get_track(uint8_t t, uint8_t __far *buf, uint16_t len)
{
    uint16_t n = 0, k;
    int16_t c, b;

    (void)t;
    if (!g_rle) {
        return (dos_read(g_fh, buf, len) == (int16_t)len)
               ? (const char *)0 : "Image file is truncated";
    }
    while (n < len) {
        if (g_run_left != 0) {                  /* owed run bytes first */
            k = len - n;
            if (k > g_run_left) k = g_run_left;
            mem_set_far(buf + n, g_run_byte, k);
            n += k;
            g_run_left -= k;
            continue;
        }
        /* Fast path: plain literals already in the scratch. */
        while (n < len && g_in_pos < g_in_len && g_scr[g_in_pos] != RLE_ESCAPE) {
            buf[n++] = g_scr[g_in_pos++];
        }
        if (n == len) break;

        if ((c = rle_in()) < 0) return "Image file is truncated";
        if (c != RLE_ESCAPE) {
            buf[n++] = (uint8_t)c;
            continue;
        }
        if ((c = rle_in()) < 0) return "Image file is truncated";
        if (c == 0) {
            buf[n++] = RLE_ESCAPE;
            continue;
        }
        if ((b = rle_in()) < 0) return "Image file is truncated";
        g_run_byte = (uint8_t)b;
        g_run_left = (uint16_t)c + 3;
    }
    return (const char *)0;
}

/* Decode the whole .RLE once before any track is written: exactly the
 * declared length, no run left over, EOF right after, and the header's CRC.
 * A truncated or corrupt file is thus refused before the floppy is touched
 * (the floppy write below takes far longer than this pass). Then rewind to
 * the first token and reset the decoder for the real pass. */
static const char *loc_preflight(uint16_t total)
{
    uint8_t ntracks = (total == SECTORS_DS) ? 160 : 80;
    uint32_t crc = FTX_CRC32_INIT;
    uint16_t first, len;
    uint8_t t;
    const char *err;

    ui_status("Checking image file...");
    for (t = 0; t < ntracks; t++) {
        len = (uint16_t)trk_geom(t, &first) * 512;
        if ((err = loc_get_track(t, g_trk, len)) != (const char *)0) {
            ui_clear_status();
            return err;
        }
        crc = ftx_crc32_update(crc, g_trk, len);
    }
    ui_clear_status();
    if (g_run_left != 0 || rle_in() >= 0) {
        return "Image file has trailing data";
    }
    if ((crc ^ FTX_CRC32_INIT) != g_hdr_crc) {
        return "Image file is corrupt (CRC)";
    }
    if (dos_seek(g_fh, RLE_HDR_SIZE, 0) != 0) {
        return "Cannot read image file";
    }
    g_in_pos = g_in_len = g_run_left = 0;
    return (const char *)0;
}

static const char *loc_close(bool_t ok, uint32_t crc)
{
    const char *err = (const char *)0;
    uint8_t b[4];

    if (g_create && ok && g_rle) {
        put_le32(b, crc);
        if (dos_seek(g_fh, 12, 0) != 0 || dos_write(g_fh, b, 4) != 4) {
            err = "Cannot write image file";
        }
    }
    if (dos_close(g_fh) != 0 && g_create) {
        err = "Cannot write image file";
    }
    if (g_create && (!ok || err != (const char *)0)) {
        dos_delete(g_lpath);
    }
    if (!g_create && ok && g_rle && crc != g_hdr_crc) {
        err = "Image CRC mismatch";
    }
    return err;
}

static const ImgIO g_local_io = {
    loc_open, loc_put_track, loc_get_track, loc_close
};
static const ImgIO g_serial_io = {
    serialfs_img_open, serialfs_img_put_track,
    serialfs_img_get_track, serialfs_img_close
};

/*---------------------------------------------------------------------------
 * Operation plumbing
 *---------------------------------------------------------------------------*/
static void bufs_free(void)
{
    mem_free(g_trk);
    mem_free(g_scr);
    g_trk = g_scr = (uint8_t __far *)0;
}

static bool_t bufs_alloc(void)
{
    g_trk = (uint8_t __far *)mem_alloc(TRK_MAX_BYTES);
    g_scr = (uint8_t __far *)mem_alloc(SCRATCH_SIZE);
    if (g_trk != (uint8_t __far *)0 && g_scr != (uint8_t __far *)0) {
        return TRUE;
    }
    bufs_free();
    dlg_alert("Error", "Not enough memory");
    return FALSE;
}

/* TRUE if the panel's listing has an entry called name (uppercase 8.3). */
static bool_t panel_has_name(Panel *p, const char *name)
{
    uint16_t i;

    for (i = 0; i < p->files.count; i++) {
        if (str_cmp(p->files.entries[i].name, name) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ESC between tracks aborts (same pattern as the serial tree copy). */
static bool_t esc_pressed(void)
{
    if (kbd_check()) {
        KeyEvent k = kbd_get();
        if (k.type == KEY_ASCII && k.code == KEY_ESC) {
            return TRUE;
        }
    }
    return FALSE;
}

/* The track loop. Floppy -> image (wr = FALSE; track 0 is already in g_trk)
 * or image -> floppy. Only between tracks is nothing in flight on the serial
 * link, which is where ESC is polled. *crc = CRC-32 of all raw track bytes. */
static bool_t run_tracks(const ImgIO *io, bool_t wr, uint8_t drive,
                         uint8_t ntracks, uint32_t *crc)
{
    const char *err;
    uint16_t first, len;
    uint8_t t;

    *crc = FTX_CRC32_INIT;
    for (t = 0; t < ntracks; t++) {
        len = (uint16_t)trk_geom(t, &first) * 512;
        if (wr) {
            if ((err = io->get_track(t, g_trk, len)) != (const char *)0) {
                dlg_alert("Error", err);
                return FALSE;
            }
            if (!disk_track(TRUE, drive, t)) return FALSE;
        } else {
            if (t > 0 && !disk_track(FALSE, drive, t)) return FALSE;
            if ((err = io->put_track(t, g_trk, len)) != (const char *)0) {
                dlg_alert("Error", err);
                return FALSE;
            }
        }
        *crc = ftx_crc32_update(*crc, g_trk, len);
        ui_progress_tick(t + 1, ntracks);
        if (t + 1 < ntracks && esc_pressed()) return FALSE;
    }
    *crc ^= FTX_CRC32_INIT;
    return TRUE;
}

/*---------------------------------------------------------------------------
 * diskimg_read_to_file - floppy -> image in the active panel's directory
 *---------------------------------------------------------------------------*/
void diskimg_read_to_file(void)
{
    Panel *p = panel_get_active();
    bool_t serial = (p->backend == PANEL_SERIAL) ? TRUE : FALSE;
    const ImgIO *io = serial ? &g_serial_io : &g_local_io;
    char name[14];
    char path[MAX_FULL_PATH];
    char msg[32];
    char title[12];
    const char *err;
    uint16_t total;
    uint32_t crc;
    bool_t ok, exists;
    int drive;

    drive = dlg_floppy_select("Read disk from");
    if (drive < 0) return;
    /* A whole image never fits on the floppy it was read from. */
    if (!serial && p->drive == (uint8_t)drive) {
        dlg_alert("Error", "Image cannot go on the disk being read");
        return;
    }

    str_copy(name, "DISK.IMG");
    if (dlg_input("Disk image", "File name:", name, 13) != DLG_OK || name[0] == '\0') {
        return;
    }
    str_upper(name);
    if (img_ext(name) == 0) {
        dlg_alert("Error", "Use .IMG or .RLE");
        return;
    }
    /* The server cannot be asked whether the name exists, but its listing is
     * what the serial panel shows, so check that instead. */
    if (serial) {
        str_copy(path, name);       /* server stores it in the listed dir */
        exists = panel_has_name(p, name);
    } else {
        path_build(path, p->drive, p->path, name);
        exists = dos_exists(path);
    }
    if (exists) {
        str_copy(msg, "Overwrite ");
        str_copy(msg + str_len(msg), name);
        str_copy(msg + str_len(msg), "?");
        if (dlg_confirm("Disk image", msg) != DLG_YES) return;
    }

    if (!bufs_alloc()) return;
    dos_disk_reset();

    /* Track 0 first: its label says how many tracks there are. */
    if (!disk_track(FALSE, (uint8_t)drive, 0)) {
        bufs_free();
        return;
    }
    total = LABEL_SECTORS(g_trk);
    if ((err = io->open(TRUE, path, &total)) != (const char *)0) {
        dlg_alert("Error", err);
        bufs_free();
        return;
    }

    str_copy(title, "Reading A:");
    title[8] = (char)('A' + drive);
    ui_show_progress(title, name, 0, (total == SECTORS_DS) ? 160 : 80);
    ok = run_tracks(io, FALSE, (uint8_t)drive,
                    (uint8_t)((total == SECTORS_DS) ? 160 : 80), &crc);
    err = io->close(ok, ok ? crc : 0);
    ui_hide_progress();
    if (ok && err != (const char *)0) {
        dlg_alert("Error", err);
    }
    bufs_free();
    panel_read_dir(p);
}

/*---------------------------------------------------------------------------
 * diskimg_write_from_file - image under the cursor -> floppy
 *---------------------------------------------------------------------------*/
void diskimg_write_from_file(void)
{
    Panel *p = panel_get_active();
    bool_t serial = (p->backend == PANEL_SERIAL) ? TRUE : FALSE;
    const ImgIO *io = serial ? &g_serial_io : &g_local_io;
    FileEntry __far *f = panel_get_cursor_file(p);
    char name[14];
    char path[MAX_FULL_PATH];
    char msg[32];
    char title[12];
    const char *err;
    uint16_t total = 0;
    uint8_t ntracks;
    uint32_t crc;
    bool_t ok;
    int drive;

    if (f == (FileEntry __far *)0 || file_is_dir(f)) {
        dlg_alert("Error", "Not a Victor disk image");
        return;
    }
    str_copy_n(name, f->name, 13);
    name[13] = '\0';
    if (img_ext(name) == 0) {
        dlg_alert("Error", "Not a Victor disk image");
        return;
    }
    if (serial) {
        serialfs_build_rel(p, name, path);
    } else {
        path_build(path, p->drive, p->path, name);
    }

    if (!bufs_alloc()) return;
    err = io->open(FALSE, path, &total);
    if (err == (const char *)0 && total != SECTORS_SS && total != SECTORS_DS) {
        io->close(FALSE, 0);
        err = "Not a Victor disk image";
    }
    if (err != (const char *)0) {
        dlg_alert("Error", err);
        bufs_free();
        return;
    }
    ntracks = (total == SECTORS_DS) ? 160 : 80;

    /* Pick and vet the target. err = "" means cancelled without a message. */
    drive = dlg_floppy_select("Write disk to");
    if (drive < 0) {
        err = "";
    } else if ((uint8_t)drive == g_exe_drive) {
        err = "Cannot overwrite the disk IGC runs from";
    } else if (!serial && p->drive == (uint8_t)drive) {
        err = "The image file is on that disk";
    } else {
        /* The GCR format cannot be laid down through DOS, so the target must
         * already be a formatted Victor disk of the same sidedness. */
        dos_disk_reset();
        if (!disk_io(FALSE, (uint8_t)drive, 0)) {
            err = "Target disk unreadable - is it formatted?";
        } else if (LABEL_SECTORS(g_trk) != total) {
            err = "Disk and image are different sidedness";
        } else {
            str_copy(msg, "Overwrite everything on A:?");
            msg[24] = (char)('A' + drive);
            if (dlg_confirm("Write disk", msg) != DLG_YES) {
                err = "";
            }
        }
    }
    if (err != (const char *)0) {
        if (err[0] != '\0') {
            dlg_alert("Error", err);
        }
        io->close(FALSE, 0);
        bufs_free();
        return;
    }

    str_copy(title, "Writing A:");
    title[8] = (char)('A' + drive);
    ui_show_progress(title, name, 0, ntracks);
    ok = run_tracks(io, TRUE, (uint8_t)drive, ntracks, &crc);
    err = io->close(ok, ok ? crc : 0);
    ui_hide_progress();
    dos_disk_reset();               /* drop DOS's view of the old disk */
    if (ok && err != (const char *)0) {
        dlg_alert("Warning", err);  /* the floppy is already written */
    }
    bufs_free();

    /* Re-read any panel on that drive from the root: its old directory may
     * not exist on the new contents. */
    if (g_left_panel.backend == PANEL_DOS && g_left_panel.drive == (uint8_t)drive) {
        panel_set_drive(&g_left_panel, (uint8_t)drive);
    }
    if (g_right_panel.backend == PANEL_DOS && g_right_panel.drive == (uint8_t)drive) {
        panel_set_drive(&g_right_panel, (uint8_t)drive);
    }
}
