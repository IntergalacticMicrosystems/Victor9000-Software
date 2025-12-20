/*
 * util.h - IGC Utility Functions
 * String, path, and memory helpers
 */

#ifndef UTIL_H
#define UTIL_H

#include "igc.h"

/*---------------------------------------------------------------------------
 * String Operations
 *---------------------------------------------------------------------------*/

/* Get string length */
uint16_t str_len(const char *s);

/* Copy string */
void str_copy(char *dst, const char *src);

/* Copy string with max length */
void str_copy_n(char *dst, const char *src, uint16_t maxlen);

/* Compare strings (returns 0 if equal) */
int str_cmp(const char *s1, const char *s2);

/* Compare strings case-insensitive */
int str_cmp_i(const char *s1, const char *s2);

/* Convert character to uppercase */
char char_upper(char c);

/* Convert string to uppercase in-place */
void str_upper(char *s);

/* Find last occurrence of character in string */
char *str_find_last(const char *s, char c);

/*---------------------------------------------------------------------------
 * Path Operations
 *---------------------------------------------------------------------------*/

/* Build full path: "D:\path\filename" */
void path_build(char *buf, uint8_t drive, const char *path, const char *filename);

/* Append component to path (adds backslash if needed) */
void path_append(char *path, const char *component);

/* Get parent directory (modifies path in-place) */
void path_get_parent(char *path);

/* Get filename from path (returns pointer into path) */
const char *path_get_filename(const char *path);

/* Get basename from path (alias for path_get_filename) */
const char *path_basename(const char *path);

/* Check if path is root directory */
bool_t path_is_root(const char *path);

/*---------------------------------------------------------------------------
 * Number Formatting
 *---------------------------------------------------------------------------*/

/* Format number without commas (e.g., "1234567") */
void num_format_simple(char *buf, uint32_t num);

/* Format number with commas (e.g., "1,234,567") */
void num_format(char *buf, uint32_t num);

/* Format file size with K/M suffix */
void size_format(char *buf, uint32_t size);

/*---------------------------------------------------------------------------
 * Memory Operations (far pointers)
 *---------------------------------------------------------------------------*/

/* Copy memory (far) */
void mem_copy_far(void __far *dst, const void __far *src, uint16_t count);

/* Set memory (far) */
void mem_set_far(void __far *dst, uint8_t val, uint16_t count);

#endif /* UTIL_H */
