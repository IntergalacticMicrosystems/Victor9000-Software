/*
 * dosmem.c - Victor 9000 DOS memory queries
 *
 * Pure INT 21h: AH=30h (version), AH=48h (largest free block) and the
 * undocumented AH=52h list-of-lists, from which the first MCB segment is
 * read. The MCB chain is then walked with far peeks.
 */

#include <dos.h>
#include <i86.h>
#include "dosmem.h"

/*---------------------------------------------------------------------------
 * Small far-peek helpers. The MCB chain lives in arbitrary segments, so we
 * always access it through far pointers regardless of memory model.
 *---------------------------------------------------------------------------*/
static uint8_t peekb(uint16_t seg, uint16_t off)
{
    return *(uint8_t __far *)MK_FP(seg, off);
}

static uint16_t peekw(uint16_t seg, uint16_t off)
{
    return *(uint16_t __far *)MK_FP(seg, off);
}

/*---------------------------------------------------------------------------
 * dosmem_version - INT 21h AH=30h
 *---------------------------------------------------------------------------*/
uint16_t dosmem_version(void)
{
    union REGS regs;

    regs.h.ah = 0x30;
    int86(0x21, &regs, &regs);

    /* AL = major, AH = minor */
    return regs.x.ax;
}

/*---------------------------------------------------------------------------
 * dosmem_largest_free_para - INT 21h AH=48h with BX=FFFFh
 *
 * The allocation is expected to fail (carry set); DOS returns the largest
 * available block size in BX. This is the same trick as igc's mem.c.
 *---------------------------------------------------------------------------*/
uint32_t dosmem_largest_free_para(void)
{
    union REGS regs;

    regs.h.ah = 0x48;
    regs.x.bx = 0xFFFF;
    int86(0x21, &regs, &regs);

    return (uint32_t)regs.x.bx;
}

/*---------------------------------------------------------------------------
 * dosmem_first_mcb - INT 21h AH=52h
 *
 * Returns ES:BX -> DOS list-of-lists. The segment of the first MCB is the
 * word stored two bytes before that structure (ES:[BX-2]).
 *---------------------------------------------------------------------------*/
void dosmem_sysvars(uint16_t *seg, uint16_t *off)
{
    union REGS regs;
    struct SREGS sregs;

    segread(&sregs);
    regs.h.ah = 0x52;
    int86x(0x21, &regs, &regs, &sregs);

    *seg = sregs.es;
    *off = regs.x.bx;
}

uint16_t dosmem_first_mcb(void)
{
    uint16_t seg, off;

    dosmem_sysvars(&seg, &off);

    /* list-of-lists; first MCB seg at [BX-2] */
    return peekw(seg, (uint16_t)(off - 2));
}

/*---------------------------------------------------------------------------
 * classify_owner - map a raw owner PSP value to a BLK_* type
 *---------------------------------------------------------------------------*/
static uint8_t classify_owner(uint16_t owner)
{
    if (owner == MCB_OWNER_FREE) {
        return BLK_FREE;
    }
    if (owner == MCB_OWNER_DOS) {
        return BLK_DOS;
    }
    return BLK_PROGRAM;
}

/*---------------------------------------------------------------------------
 * read_name - copy the 8-byte MCB owner name (DOS 4+ only)
 *
 * On DOS < 4 this field is not maintained, so we leave the name empty and
 * let the caller fall back to owner-PSP heuristics. Even on DOS 4+ we sanity
 * check that the bytes are printable before trusting them.
 *---------------------------------------------------------------------------*/
static void read_name(uint16_t mcb_seg, uint16_t dos_major, char *out)
{
    uint8_t i;
    uint8_t c;

    out[0] = '\0';
    if (dos_major < 4) {
        return;
    }

    for (i = 0; i < 8; i++) {
        c = peekb(mcb_seg, (uint16_t)(8 + i));
        if (c == 0) {
            break;
        }
        if (c < 0x20 || c > 0x7E) {
            /* Non-printable - distrust the whole field. */
            out[0] = '\0';
            return;
        }
        out[i] = (char)c;
    }
    out[i] = '\0';
}

/*---------------------------------------------------------------------------
 * dosmem_psp_name - resolve a program basename from its PSP environment
 *
 * PSP layout used:
 *   PSP:[0]    = INT 20h opcode (CD 20) - sanity check that this is a PSP
 *   PSP:[2Ch]  = environment segment (word)
 * Environment layout (DOS 3.0+):
 *   "VAR=val\0" ... "\0"           <- string list ends with an empty string
 *   word  count                    <- number of trailing strings (>=1)
 *   "X:\PATH\PROG.EXE\0"           <- full program path
 * We extract the basename (after the last '\' or ':').
 *---------------------------------------------------------------------------*/
bool_t dosmem_psp_name(uint16_t psp_seg, char *out)
{
    uint16_t env;
    uint32_t i;
    uint16_t count;
    uint16_t start;
    uint16_t j;
    uint8_t  c;

    out[0] = '\0';

    if (psp_seg == 0) {
        return FALSE;
    }
    /* Must look like a PSP (begins with INT 20h: CD 20). */
    if (peekb(psp_seg, 0) != 0xCD || peekb(psp_seg, 1) != 0x20) {
        return FALSE;
    }

    env = peekw(psp_seg, 0x2C);
    if (env == 0) {
        return FALSE;
    }

    /* Skip the environment strings: advance until an empty string (a 0 byte
     * at the start of a string). Cap the scan so a corrupt env can't run
     * away past a 64KB segment. */
    i = 0;
    while (peekb(env, (uint16_t)i) != 0) {
        while (i < 0x7FF0L && peekb(env, (uint16_t)i) != 0) {
            i++;
        }
        i++;                            /* skip this string's NUL */
        if (i >= 0x7FF0L) {
            return FALSE;
        }
    }
    i++;                                /* skip the terminating empty string */

    count = peekw(env, (uint16_t)i);
    i += 2;
    if (count < 1) {
        return FALSE;                   /* no program path present */
    }

    /* env:i now points at the ASCIIZ program path. Find its basename. */
    start = (uint16_t)i;
    for (j = 0; j < 128; j++) {
        c = peekb(env, (uint16_t)(i + j));
        if (c == 0) {
            break;
        }
        if (c == '\\' || c == '/' || c == ':') {
            start = (uint16_t)(i + j + 1);
        }
    }

    /* Copy the basename (8.3 -> max 12 chars + NUL). */
    for (j = 0; j < 12; j++) {
        c = peekb(env, (uint16_t)(start + j));
        if (c == 0) {
            break;
        }
        out[j] = (char)c;
    }
    out[j] = '\0';

    return (out[0] != '\0') ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * dosmem_walk_devices - walk the DOS device-driver chain
 *
 * The chain starts at the NUL device embedded in the list-of-lists. Its
 * offset within SysVars depends on the DOS version:
 *   DOS 2.x   -> 0x17    DOS 3.0 -> 0x28    DOS 3.1+ -> 0x22
 * Each header: [0]=next off, [2]=next seg (off 0xFFFF = end), [4]=attr,
 * [0Ah]=8-char name (char devices) or unit count (block devices).
 *---------------------------------------------------------------------------*/
uint16_t dosmem_walk_devices(uint16_t dos_version,
                             DeviceDriver *drv, uint16_t max)
{
    uint16_t sv_seg, sv_off;
    uint16_t seg, off;
    uint16_t nul_off;
    uint16_t major, minor;
    uint16_t count;
    uint8_t  i;
    int      k;

    major = (uint16_t)(dos_version & 0x00FF);
    minor = (uint16_t)((dos_version >> 8) & 0x00FF);

    if (major <= 2) {
        nul_off = 0x17;
    } else if (major == 3 && minor == 0) {
        nul_off = 0x28;
    } else {
        nul_off = 0x22;             /* DOS 3.1+ (the Victor) */
    }

    dosmem_sysvars(&sv_seg, &sv_off);
    seg = sv_seg;
    off = (uint16_t)(sv_off + nul_off);

    count = 0;
    for (;;) {
        DeviceDriver d;
        uint16_t attr = peekw(seg, (uint16_t)(off + 4));

        d.seg = seg;
        d.off = off;
        d.attr = attr;
        d.is_char = (attr & DEV_ATTR_CHAR) ? TRUE : FALSE;
        d.units = 0;
        d.name[0] = '\0';

        if (d.is_char) {
            for (i = 0; i < 8; i++) {
                d.name[i] = (char)peekb(seg, (uint16_t)(off + 0x0A + i));
            }
            d.name[8] = '\0';
            /* Trim trailing spaces. */
            for (k = 7; k >= 0 && (d.name[k] == ' ' || d.name[k] == 0); k--) {
                d.name[k] = '\0';
            }
        } else {
            d.units = peekb(seg, (uint16_t)(off + 0x0A));
        }

        if (drv != (DeviceDriver *)0 && count < max) {
            drv[count] = d;
        }
        count++;

        /* Advance to the next driver. */
        {
            uint16_t next_off = peekw(seg, off);
            uint16_t next_seg = peekw(seg, (uint16_t)(off + 2));
            if (next_off == 0xFFFF || count >= 0xFFF0) {
                break;
            }
            seg = next_seg;
            off = next_off;
        }
    }

    return count;
}

/*---------------------------------------------------------------------------
 * dosmem_walk - walk the MCB chain
 *---------------------------------------------------------------------------*/
uint16_t dosmem_walk(MemReport *rep, MemBlock *blocks, uint16_t max_blocks)
{
    uint16_t dos_major;
    uint16_t seg;
    uint16_t count;
    uint8_t  mark;
    MemBlock blk;

    rep->dos_version = dosmem_version();
    rep->first_mcb = dosmem_first_mcb();
    rep->largest_free_para = dosmem_largest_free_para();
    rep->total_para = 0;
    rep->used_para = 0;
    rep->free_para = 0;
    rep->block_count = 0;
    rep->chain_ok = TRUE;

    dos_major = (uint16_t)(rep->dos_version & 0x00FF);

    /* Memory below the first MCB (interrupt vectors, DOS, BIOS data). */
    rep->total_para = (uint32_t)rep->first_mcb;

    seg = rep->first_mcb;
    count = 0;

    for (;;) {
        mark = peekb(seg, 0);
        if (mark != MCB_MARK_MORE && mark != MCB_MARK_LAST) {
            /* Chain is corrupt or this DOS lays memory out differently. */
            rep->chain_ok = FALSE;
            break;
        }

        blk.mcb_seg = seg;
        blk.data_seg = (uint16_t)(seg + 1);
        blk.owner = peekw(seg, 1);
        blk.size_para = peekw(seg, 3);
        blk.type = classify_owner(blk.owner);
        blk.is_last = (mark == MCB_MARK_LAST);
        read_name(seg, dos_major, blk.name);

        /* Resolve a real program name from the owner PSP's environment
         * (works on DOS 3.1, unlike the MCB name field). */
        blk.prog[0] = '\0';
        if (blk.type == BLK_PROGRAM) {
            dosmem_psp_name(blk.owner, blk.prog);
        }

        /* Totals include the one-paragraph MCB header for each block. */
        rep->total_para += (uint32_t)blk.size_para + 1L;
        if (blk.type == BLK_FREE) {
            rep->free_para += (uint32_t)blk.size_para + 1L;
        } else {
            rep->used_para += (uint32_t)blk.size_para + 1L;
        }

        if (blocks != (MemBlock *)0 && count < max_blocks) {
            blocks[count] = blk;
        }
        count++;

        if (blk.is_last) {
            break;
        }

        /* Next MCB sits immediately after this block. */
        seg = (uint16_t)(seg + 1 + blk.size_para);
    }

    rep->block_count = count;
    return count;
}
