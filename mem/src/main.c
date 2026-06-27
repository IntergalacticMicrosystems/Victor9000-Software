/*
 * main.c - MEM: Victor 9000 DOS memory reporter
 *
 * Prints a conventional-memory summary to stdout. With -m it also dumps the
 * DOS memory-control-block (MCB) chain. Plain text so it can be redirected:
 *
 *     MEM            summary only
 *     MEM -M         summary + per-block MCB chain
 *     MEM > REPORT.TXT
 *
 * Target: Victor 9000 / DOS 3.1, Open Watcom v2 (compact model, 8086).
 */

#include <stdio.h>
#include <string.h>
#include "dosmem.h"

/* Up to this many blocks are listed in -m mode. A real DOS 3.1 machine has
 * well under this; extra blocks are still counted in the totals. */
#define MAX_BLOCKS  128
#define MAX_DEVICES 64

/*---------------------------------------------------------------------------
 * Formatting helpers
 *---------------------------------------------------------------------------*/

/* Paragraphs (16 bytes) -> whole KB, rounded to nearest. */
static uint32_t para_to_kb(uint32_t para)
{
    return (para * 16L + 512L) / 1024L;
}

/* A short, stable label for a block's owner. */
static const char *owner_label(const MemBlock *b, uint16_t first_mcb)
{
    if (b->type == BLK_FREE) {
        return "free";
    }
    if (b->type == BLK_DOS) {
        return "DOS";
    }
    if (b->prog[0] != '\0') {
        return b->prog;       /* name from the PSP environment (DOS 3+) */
    }
    if (b->name[0] != '\0') {
        return b->name;       /* MCB owner name (DOS 4+) */
    }
    /* Last resort: the first MCB is DOS system data. */
    if (b->mcb_seg == (uint16_t)(first_mcb)) {
        return "(system)";
    }
    return "(program)";
}

/*---------------------------------------------------------------------------
 * Report sections
 *---------------------------------------------------------------------------*/

static void print_summary(const MemReport *rep)
{
    uint32_t total_kb = para_to_kb(rep->total_para);
    uint32_t used_kb  = para_to_kb(rep->used_para);
    uint32_t free_kb  = para_to_kb(rep->free_para);
    uint32_t large_kb = para_to_kb(rep->largest_free_para);

    printf("Conventional memory:\n");
    printf("  Total      : %lu KB\n", total_kb);
    printf("  Used       : %lu KB\n", used_kb);
    printf("  Free       : %lu KB\n", free_kb);
    printf("  Largest    : %lu KB\n", large_kb);
    printf("DOS version %u.%02u\n",
           (unsigned)(rep->dos_version & 0x00FF),
           (unsigned)((rep->dos_version >> 8) & 0x00FF));

    if (!rep->chain_ok) {
        printf("\nWARNING: MCB chain ended unexpectedly; totals may be"
               " incomplete.\n");
    }
}

static void print_chain(const MemBlock *blocks, uint16_t shown,
                        const MemReport *rep)
{
    uint16_t i;
    uint32_t kb;

    printf("\nMCB chain:\n");
    printf("  SEG   OWNER  SIZE      TYPE/NAME\n");

    for (i = 0; i < shown; i++) {
        const MemBlock *b = &blocks[i];
        kb = para_to_kb((uint32_t)b->size_para);
        printf("  %04X  %04X   %6lu KB %s\n",
               b->mcb_seg, b->owner, kb, owner_label(b, rep->first_mcb));
    }

    if (rep->block_count > shown) {
        printf("  ... %u more block(s) not shown\n",
               (unsigned)(rep->block_count - shown));
    }
}

static void print_devices(const DeviceDriver *drv, uint16_t count)
{
    uint16_t i;

    printf("\nDevice drivers:\n");
    printf("  ADDRESS    ATTR  TYPE   NAME/UNITS\n");
    for (i = 0; i < count; i++) {
        const DeviceDriver *d = &drv[i];
        if (d->is_char) {
            printf("  %04X:%04X  %04X  char   %s\n",
                   d->seg, d->off, d->attr, d->name);
        } else {
            printf("  %04X:%04X  %04X  block  %u unit(s)\n",
                   d->seg, d->off, d->attr, (unsigned)d->units);
        }
    }
}

static void print_usage(void)
{
    printf("MEM - Victor 9000 memory report\n");
    printf("Usage: MEM [-M] [-D] [-?]\n");
    printf("  -M   also list the DOS memory-control-block chain\n");
    printf("  -D   also list the DOS device-driver chain\n");
    printf("  -?   show this help\n");
}

/*---------------------------------------------------------------------------
 * Argument parsing - accept -X and /X, case-insensitive.
 *---------------------------------------------------------------------------*/
static char opt_char(const char *arg)
{
    char c;

    if (arg[0] != '-' && arg[0] != '/') {
        return 0;
    }
    c = arg[1];
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    return c;
}

int main(int argc, char *argv[])
{
    /* These arrays are large (several KB) - keep them out of the stack. */
    static MemBlock     blocks[MAX_BLOCKS];
    static DeviceDriver devices[MAX_DEVICES];
    MemReport    rep;
    uint16_t     shown;
    uint16_t     dev_count;
    int          i;
    bool_t       want_chain = FALSE;
    bool_t       want_devs = FALSE;

    for (i = 1; i < argc; i++) {
        switch (opt_char(argv[i])) {
        case 'M':
            want_chain = TRUE;
            break;
        case 'D':
            want_devs = TRUE;
            break;
        case '?':
        case 'H':
            print_usage();
            return 0;
        default:
            printf("Unknown option: %s\n\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    dosmem_walk(&rep, blocks, MAX_BLOCKS);

    print_summary(&rep);

    if (want_chain) {
        shown = rep.block_count;
        if (shown > MAX_BLOCKS) {
            shown = MAX_BLOCKS;
        }
        print_chain(blocks, shown, &rep);
    }

    if (want_devs) {
        dev_count = dosmem_walk_devices(rep.dos_version, devices, MAX_DEVICES);
        if (dev_count > MAX_DEVICES) {
            dev_count = MAX_DEVICES;
        }
        print_devices(devices, dev_count);
    }

    return 0;
}
