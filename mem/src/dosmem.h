/*
 * dosmem.h - Victor 9000 DOS memory queries
 *
 * All information is obtained through DOS (INT 21h) only. The Victor 9000
 * BIOS is not IBM-compatible, so INT 12h is deliberately not used - the
 * same choice igc/src/mem.c makes (it relies only on INT 21h AH=48h).
 */

#ifndef DOSMEM_H
#define DOSMEM_H

/*---------------------------------------------------------------------------
 * Fixed-width types (mirrors igc.h - this tool is self-contained)
 *---------------------------------------------------------------------------*/
typedef unsigned char   uint8_t;
typedef unsigned short  uint16_t;
typedef unsigned long   uint32_t;

typedef uint8_t bool_t;
#define TRUE    1
#define FALSE   0

/*---------------------------------------------------------------------------
 * Memory Control Block field offsets (the MCB is undocumented but stable
 * across MS-DOS versions). We peek fields with far pointers rather than a
 * packed struct so the build needs no special -zp1 packing.
 *
 *   offset 0 : marker byte  'M' (0x4D) = more blocks, 'Z' (0x5A) = last
 *   offset 1 : owner PSP segment (word); 0 = free, 8 = DOS itself
 *   offset 3 : block size in paragraphs (word), excluding the MCB itself
 *   offset 8 : 8-char owner program name - DOS 4+ ONLY (garbage on 3.1)
 *---------------------------------------------------------------------------*/
#define MCB_MARK_MORE   0x4D    /* 'M' */
#define MCB_MARK_LAST   0x5A    /* 'Z' */
#define MCB_OWNER_FREE  0x0000
#define MCB_OWNER_DOS   0x0008

/* Classification of a block's owner */
#define BLK_FREE        0
#define BLK_DOS         1
#define BLK_PROGRAM     2

/*---------------------------------------------------------------------------
 * One block of the MCB chain, decoded.
 *---------------------------------------------------------------------------*/
typedef struct {
    uint16_t mcb_seg;       /* segment of the MCB header */
    uint16_t data_seg;      /* mcb_seg + 1 (start of usable memory) */
    uint16_t owner;         /* owner PSP segment (raw) */
    uint16_t size_para;     /* size in paragraphs (not incl. MCB) */
    uint8_t  type;          /* BLK_FREE / BLK_DOS / BLK_PROGRAM */
    char     name[9];       /* owner name (DOS 4+), or "" - NUL terminated */
    char     prog[13];      /* program basename from the PSP env (DOS 3+), or "" */
    bool_t   is_last;       /* TRUE if marker was 'Z' */
} MemBlock;

/*---------------------------------------------------------------------------
 * One DOS device driver, decoded from the driver chain.
 *
 * Attribute bit 15 (0x8000) selects character vs. block device. Character
 * devices carry an 8-byte name at header offset 0x0A; block devices store a
 * unit count there instead.
 *---------------------------------------------------------------------------*/
#define DEV_ATTR_CHAR   0x8000

typedef struct {
    uint16_t seg;           /* driver header segment */
    uint16_t off;           /* driver header offset */
    uint16_t attr;          /* attribute word */
    bool_t   is_char;       /* TRUE = character device, FALSE = block device */
    uint8_t  units;         /* block devices: number of units */
    char     name[9];       /* character devices: 8-char name, trimmed + NUL */
} DeviceDriver;

/*---------------------------------------------------------------------------
 * Aggregate report.
 *---------------------------------------------------------------------------*/
typedef struct {
    uint16_t dos_version;       /* AL=major (low byte), AH=minor (high byte) */
    uint16_t first_mcb;         /* segment of the first MCB */
    uint32_t total_para;        /* all conventional memory, in paragraphs */
    uint32_t used_para;         /* sum of owned blocks (incl. their MCBs) */
    uint32_t free_para;         /* sum of free blocks (incl. their MCBs) */
    uint32_t largest_free_para; /* largest single free block (AH=48h) */
    uint16_t block_count;       /* number of MCBs walked */
    bool_t   chain_ok;          /* FALSE if the chain looked corrupt */
} MemReport;

/*---------------------------------------------------------------------------
 * Queries
 *---------------------------------------------------------------------------*/

/* INT 21h AH=30h - returns AX (AL=major, AH=minor). */
uint16_t dosmem_version(void);

/* INT 21h AH=48h BX=FFFFh - largest free block, in paragraphs. */
uint32_t dosmem_largest_free_para(void);

/* INT 21h AH=52h - segment of the first MCB (via the list-of-lists). */
uint16_t dosmem_first_mcb(void);

/* INT 21h AH=52h - far address (seg:off) of the DOS list-of-lists. */
void dosmem_sysvars(uint16_t *seg, uint16_t *off);

/*
 * Resolve a program's basename from its PSP environment block (DOS 3.0+
 * stores the full program path after the environment strings - this works
 * even on DOS 3.1, where the MCB owner-name field does not). Writes a NUL-
 * terminated 8.3 basename into out (size >= 13). Returns TRUE on success.
 */
bool_t dosmem_psp_name(uint16_t psp_seg, char *out);

/*
 * Walk the DOS device-driver chain (starting at the NUL device inside the
 * list-of-lists). Stores up to max drivers; returns the number found.
 */
uint16_t dosmem_walk_devices(uint16_t dos_version,
                             DeviceDriver *drv, uint16_t max);

/*
 * Walk the MCB chain. Fills rep with aggregate totals. If blocks != NULL,
 * stores up to max_blocks decoded MemBlock entries. Returns the number of
 * blocks found (which may exceed max_blocks; rep->block_count has the same).
 */
uint16_t dosmem_walk(MemReport *rep, MemBlock *blocks, uint16_t max_blocks);

#endif /* DOSMEM_H */
