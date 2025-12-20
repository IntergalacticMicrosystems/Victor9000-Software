/*
 * mem.h - IGC Memory Management
 * Dynamic allocation with runtime scaling based on available RAM
 */

#ifndef MEM_H
#define MEM_H

#include "igc.h"

/*---------------------------------------------------------------------------
 * Memory State
 *---------------------------------------------------------------------------*/
typedef struct {
    uint32_t total_kb;          /* Total available KB at startup */
    uint32_t free_kb;           /* Current free KB */
    uint8_t  tier;              /* MEM_LOW, MEM_MEDIUM, or MEM_HIGH */
    uint16_t files_per_panel;   /* Max files based on tier */
    uint32_t editor_buf_size;   /* Editor buffer size based on tier */
    uint16_t copy_buf_size;     /* Copy buffer size based on tier */
} MemState;

extern MemState g_mem;

/*---------------------------------------------------------------------------
 * Initialization
 *---------------------------------------------------------------------------*/

/* Initialize memory system, detect available RAM, set tier */
void mem_init(void);

/* Shutdown - free all tracked allocations */
void mem_shutdown(void);

/*---------------------------------------------------------------------------
 * Memory Information
 *---------------------------------------------------------------------------*/

/* Get available conventional memory in KB */
uint32_t mem_get_available_kb(void);

/* Get current memory tier */
uint8_t mem_get_tier(void);

/* Get tier name for display */
const char *mem_get_tier_name(void);

/*---------------------------------------------------------------------------
 * Allocation Functions
 * These wrap DOS memory allocation for far heap
 *---------------------------------------------------------------------------*/

/* Allocate far memory block */
void __far *mem_alloc(uint32_t bytes);

/* Free far memory block */
void mem_free(void __far *ptr);

/* Reallocate far memory block (may move) */
void __far *mem_realloc(void __far *ptr, uint32_t old_size, uint32_t new_size);

/*---------------------------------------------------------------------------
 * Tier-Based Limits
 *---------------------------------------------------------------------------*/

/* Get max files per panel for current tier */
uint16_t mem_get_files_per_panel(void);

/* Get editor buffer size for current tier */
uint32_t mem_get_editor_buf_size(void);

/* Get copy buffer size for current tier */
uint16_t mem_get_copy_buf_size(void);

#endif /* MEM_H */
