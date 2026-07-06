/*
 * mem.c - IGC Memory Management Implementation
 * Dynamic allocation with runtime scaling based on available RAM
 */

#include <dos.h>
#include <i86.h>
#include "mem.h"

/*---------------------------------------------------------------------------
 * Global Memory State
 *---------------------------------------------------------------------------*/
MemState g_mem;

/*---------------------------------------------------------------------------
 * mem_get_available_kb - Query DOS for available conventional memory
 *
 * Uses INT 21h AH=48h to try allocating maximum memory, then releases it.
 * This gives us the largest free block in paragraphs (16-byte units).
 *---------------------------------------------------------------------------*/
uint32_t mem_get_available_kb(void)
{
    union REGS regs;
    uint16_t max_paragraphs;
    uint32_t bytes;

    /* Try to allocate 0xFFFF paragraphs (maximum possible) */
    /* DOS will fail but return max available in BX */
    regs.h.ah = 0x48;
    regs.x.bx = 0xFFFF;
    int86(0x21, &regs, &regs);

    /* BX now contains maximum available paragraphs */
    max_paragraphs = regs.x.bx;

    /* Convert paragraphs to KB (paragraph = 16 bytes) */
    bytes = (uint32_t)max_paragraphs * 16L;

    return bytes / 1024L;
}

/*---------------------------------------------------------------------------
 * mem_init - Initialize memory system
 *---------------------------------------------------------------------------*/
void mem_init(void)
{
    /* Get available memory */
    g_mem.total_kb = mem_get_available_kb();

    /* Determine tier based on available memory. (The editor sizes its own
     * buffers from the tier - see EDIT_BUF_* in editor.h.) */
    if (g_mem.total_kb >= MEM_HIGH_THRESHOLD) {
        g_mem.tier = MEM_HIGH;
        g_mem.files_per_panel = FILES_PER_PANEL_HIGH;
        g_mem.copy_buf_size = COPY_BUF_HIGH;
        g_mem.xfer_buf_size = XFER_BUF_HIGH;
    } else if (g_mem.total_kb >= MEM_MEDIUM_THRESHOLD) {
        g_mem.tier = MEM_MEDIUM;
        g_mem.files_per_panel = FILES_PER_PANEL_MEDIUM;
        g_mem.copy_buf_size = COPY_BUF_MEDIUM;
        g_mem.xfer_buf_size = XFER_BUF_MEDIUM;
    } else if (g_mem.total_kb >= MEM_LOW_THRESHOLD) {
        g_mem.tier = MEM_LOW;
        g_mem.files_per_panel = FILES_PER_PANEL_LOW;
        g_mem.copy_buf_size = COPY_BUF_LOW;
        g_mem.xfer_buf_size = XFER_BUF_LOW;
    } else {
        g_mem.tier = MEM_TINY;
        g_mem.files_per_panel = FILES_PER_PANEL_TINY;
        g_mem.copy_buf_size = COPY_BUF_TINY;
        g_mem.xfer_buf_size = XFER_BUF_TINY;
    }
}

/*---------------------------------------------------------------------------
 * mem_shutdown - Cleanup memory system
 *---------------------------------------------------------------------------*/
void mem_shutdown(void)
{
    /* Nothing to do - individual allocations should be freed by callers */
    /* This is a placeholder for any global cleanup needed */
}

/*---------------------------------------------------------------------------
 * mem_get_tier - Get current memory tier
 *---------------------------------------------------------------------------*/
uint8_t mem_get_tier(void)
{
    return g_mem.tier;
}

/*---------------------------------------------------------------------------
 * mem_alloc - Allocate far memory block
 *
 * Uses DOS INT 21h AH=48h to allocate memory.
 * Returns far pointer or NULL on failure.
 *---------------------------------------------------------------------------*/
void __far *mem_alloc(uint32_t bytes)
{
    union REGS regs;
    uint16_t paragraphs;
    uint16_t segment;

    /* Calculate paragraphs needed (round up) */
    paragraphs = (uint16_t)((bytes + 15L) / 16L);

    /* DOS allocate memory */
    regs.h.ah = 0x48;
    regs.x.bx = paragraphs;
    int86(0x21, &regs, &regs);

    /* Check for error (carry flag set) */
    if (regs.x.cflag) {
        return (void __far *)0;
    }

    /* AX contains segment of allocated block */
    segment = regs.x.ax;

    /* Return far pointer (segment:0000) */
    return MK_FP(segment, 0);
}

/*---------------------------------------------------------------------------
 * mem_free - Free far memory block
 *---------------------------------------------------------------------------*/
void mem_free(void __far *ptr)
{
    union REGS regs;
    struct SREGS sregs;

    if (ptr == (void __far *)0) {
        return;
    }

    /* Get segment from far pointer */
    segread(&sregs);
    sregs.es = FP_SEG(ptr);

    /* DOS free memory */
    regs.h.ah = 0x49;
    int86x(0x21, &regs, &regs, &sregs);
}

/*---------------------------------------------------------------------------
 * Tier-Based Limit Accessors
 *---------------------------------------------------------------------------*/

uint16_t mem_get_files_per_panel(void)
{
    return g_mem.files_per_panel;
}

uint16_t mem_get_copy_buf_size(void)
{
    return g_mem.copy_buf_size;
}

uint16_t mem_get_xfer_buf_size(void)
{
    return g_mem.xfer_buf_size;
}
