/*
 * util.c - IGC Utility Functions Implementation
 */

#include "util.h"

/*---------------------------------------------------------------------------
 * str_len - Get string length
 *---------------------------------------------------------------------------*/
uint16_t str_len(const char *s)
{
    uint16_t len = 0;
    while (*s++) len++;
    return len;
}

/*---------------------------------------------------------------------------
 * str_copy - Copy string
 *---------------------------------------------------------------------------*/
void str_copy(char *dst, const char *src)
{
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

/*---------------------------------------------------------------------------
 * str_copy_n - Copy string with max length
 *---------------------------------------------------------------------------*/
void str_copy_n(char *dst, const char *src, uint16_t maxlen)
{
    uint16_t i = 0;
    while (*src && i < maxlen - 1) {
        *dst++ = *src++;
        i++;
    }
    *dst = '\0';
}

/*---------------------------------------------------------------------------
 * str_cmp - Compare strings
 *---------------------------------------------------------------------------*/
int str_cmp(const char *s1, const char *s2)
{
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

/*---------------------------------------------------------------------------
 * char_upper - Convert character to uppercase
 *---------------------------------------------------------------------------*/
char char_upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return c - 32;
    }
    return c;
}

/*---------------------------------------------------------------------------
 * str_cmp_i - Compare strings case-insensitive
 *---------------------------------------------------------------------------*/
int str_cmp_i(const char *s1, const char *s2)
{
    char c1, c2;
    while (*s1 && *s2) {
        c1 = char_upper(*s1);
        c2 = char_upper(*s2);
        if (c1 != c2) {
            return (int)(unsigned char)c1 - (int)(unsigned char)c2;
        }
        s1++;
        s2++;
    }
    return (int)(unsigned char)char_upper(*s1) - (int)(unsigned char)char_upper(*s2);
}

/*---------------------------------------------------------------------------
 * str_upper - Convert string to uppercase in-place
 *---------------------------------------------------------------------------*/
void str_upper(char *s)
{
    while (*s) {
        *s = char_upper(*s);
        s++;
    }
}

/*---------------------------------------------------------------------------
 * str_find_last - Find last occurrence of character
 *---------------------------------------------------------------------------*/
char *str_find_last(const char *s, char c)
{
    const char *last = (const char *)0;
    while (*s) {
        if (*s == c) {
            last = s;
        }
        s++;
    }
    return (char *)last;
}

/*---------------------------------------------------------------------------
 * path_build - Build full path "D:\path\filename"
 *---------------------------------------------------------------------------*/
void path_build(char *buf, uint8_t drive, const char *path, const char *filename)
{
    /* Drive letter */
    *buf++ = 'A' + drive;
    *buf++ = ':';
    *buf++ = '\\';

    /* Path (skip leading backslash if present) */
    if (*path == '\\') path++;
    while (*path) {
        *buf++ = *path++;
    }

    /* Add backslash before filename if path is not empty */
    if (buf[-1] != '\\') {
        *buf++ = '\\';
    }

    /* Filename */
    while (*filename) {
        *buf++ = *filename++;
    }

    *buf = '\0';
}

/*---------------------------------------------------------------------------
 * path_append - Append component to path
 *---------------------------------------------------------------------------*/
void path_append(char *path, const char *component)
{
    uint16_t len = str_len(path);

    /* Add backslash if needed */
    if (len > 0 && path[len - 1] != '\\') {
        path[len++] = '\\';
    }

    /* Copy component */
    while (*component) {
        path[len++] = *component++;
    }
    path[len] = '\0';
}

/*---------------------------------------------------------------------------
 * path_get_parent - Get parent directory (modifies in-place)
 *---------------------------------------------------------------------------*/
void path_get_parent(char *path)
{
    char *last_slash;

    /* Find last backslash */
    last_slash = str_find_last(path, '\\');

    if (last_slash != (char *)0) {
        /* If it's the first character, keep root */
        if (last_slash == path) {
            path[1] = '\0';
        } else {
            *last_slash = '\0';
        }
    } else {
        /* No backslash - set to empty (root) */
        path[0] = '\0';
    }
}

/*---------------------------------------------------------------------------
 * path_get_filename - Get filename from path
 *---------------------------------------------------------------------------*/
const char *path_get_filename(const char *path)
{
    const char *last_slash = str_find_last(path, '\\');
    if (last_slash != (const char *)0) {
        return last_slash + 1;
    }
    return path;
}

/*---------------------------------------------------------------------------
 * path_basename - Get basename from path (alias)
 *---------------------------------------------------------------------------*/
const char *path_basename(const char *path)
{
    return path_get_filename(path);
}

/*---------------------------------------------------------------------------
 * path_is_root - Check if path is root directory
 *---------------------------------------------------------------------------*/
bool_t path_is_root(const char *path)
{
    /* Empty path or just backslash */
    if (path[0] == '\0') return TRUE;
    if (path[0] == '\\' && path[1] == '\0') return TRUE;
    return FALSE;
}

/*---------------------------------------------------------------------------
 * num_format_simple - Format number without commas
 *---------------------------------------------------------------------------*/
void num_format_simple(char *buf, uint32_t num)
{
    char temp[12];
    int i = 0;
    int j = 0;

    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    /* Convert to string (reversed) */
    while (num > 0) {
        temp[i++] = '0' + (char)(num % 10L);
        num /= 10L;
    }

    /* Copy in correct order */
    while (i > 0) {
        i--;
        buf[j++] = temp[i];
    }
    buf[j] = '\0';
}

/*---------------------------------------------------------------------------
 * num_format - Format number with commas
 *---------------------------------------------------------------------------*/
void num_format(char *buf, uint32_t num)
{
    char temp[15];
    int i = 0;
    int j = 0;
    int digits = 0;

    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    /* Convert to string (reversed) */
    while (num > 0) {
        temp[i++] = '0' + (char)(num % 10L);
        num /= 10L;
        digits++;
    }

    /* Copy with commas */
    while (i > 0) {
        i--;
        buf[j++] = temp[i];
        if (i > 0 && (digits - i) % 3 == 0) {
            buf[j++] = ',';
        }
    }
    buf[j] = '\0';
}

/*---------------------------------------------------------------------------
 * size_format - Format file size with K/M suffix
 *---------------------------------------------------------------------------*/
void size_format(char *buf, uint32_t size)
{
    if (size < 1024L) {
        /* Show bytes */
        num_format(buf, size);
    } else if (size < 1048576L) {
        /* Show KB */
        num_format(buf, size / 1024L);
        str_copy(buf + str_len(buf), "K");
    } else {
        /* Show MB */
        num_format(buf, size / 1048576L);
        str_copy(buf + str_len(buf), "M");
    }
}

/*---------------------------------------------------------------------------
 * mem_copy_far - Copy memory (far pointers)
 *---------------------------------------------------------------------------*/
void mem_copy_far(void __far *dst, const void __far *src, uint16_t count)
{
    char __far *d = (char __far *)dst;
    const char __far *s = (const char __far *)src;

    while (count--) {
        *d++ = *s++;
    }
}

/*---------------------------------------------------------------------------
 * mem_set_far - Set memory (far pointers)
 *---------------------------------------------------------------------------*/
void mem_set_far(void __far *dst, uint8_t val, uint16_t count)
{
    char __far *d = (char __far *)dst;

    while (count--) {
        *d++ = (char)val;
    }
}
