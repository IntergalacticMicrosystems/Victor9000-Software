/*
 * config.c - IGC Configuration Implementation
 */

#include "config.h"
#include "panel.h"
#include "dosapi.h"
#include "util.h"
#include "serialfs.h"

/*---------------------------------------------------------------------------
 * Configuration file path
 *
 * Defaults to the bare filename (resolved against the current working
 * directory). config_init_path() rewrites this to sit next to the executable.
 *---------------------------------------------------------------------------*/
static char config_path[MAX_PATH_LEN] = "IGC.INI";

/*---------------------------------------------------------------------------
 * config_init_path - Point IGC.INI at the executable's directory
 *
 * On DOS 3.0+ the C runtime supplies argv[0] as the fully-qualified path of
 * the running program (e.g. "A:\TOOLS\IGC.EXE"). We strip the filename and
 * append "IGC.INI" so the config file lives next to the EXE regardless of the
 * current working directory. If no directory information is available, the
 * default "IGC.INI" (current working directory) is left in place.
 *---------------------------------------------------------------------------*/
void config_init_path(const char *exe_path)
{
    const char *name;
    uint16_t dir_len;

    if (exe_path == (const char *)0 || *exe_path == '\0') {
        return;
    }

    /* path_get_filename returns a pointer to the filename portion; the bytes
     * before it (including any trailing '\' or drive ':') are the directory. */
    name = path_get_filename(exe_path);
    dir_len = (uint16_t)(name - exe_path);

    /* No directory prefix at all (e.g. DOS < 3.0 gives just a name) -> keep
     * the current-working-directory default. */
    if (dir_len == 0) {
        return;
    }

    /* Make sure the directory prefix + "IGC.INI" fits in the buffer. */
    if (dir_len + str_len("IGC.INI") + 1 > MAX_PATH_LEN) {
        return;
    }

    /* str_copy_n copies at most maxlen-1 chars, so pass dir_len+1 to keep all
     * dir_len directory bytes; it null-terminates at index dir_len. */
    str_copy_n(config_path, exe_path, dir_len + 1);
    str_copy(config_path + dir_len, "IGC.INI");
}

/*---------------------------------------------------------------------------
 * skip_whitespace - Skip whitespace in string
 *---------------------------------------------------------------------------*/
static const char *skip_whitespace(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/*---------------------------------------------------------------------------
 * key_is - Case-insensitive match of the key portion of an INI line
 *---------------------------------------------------------------------------*/
static bool_t key_is(const char *key, uint16_t key_len, const char *name)
{
    uint16_t i;

    for (i = 0; i < key_len; i++) {
        if (char_upper(key[i]) != char_upper(name[i])) {
            return FALSE;
        }
    }

    return (name[key_len] == '\0') ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * parse_drive_value - Parse a drive letter value
 *---------------------------------------------------------------------------*/
static void parse_drive_value(uint8_t *drive, const char *value)
{
    char c = char_upper(*value);

    if (c >= 'A' && c <= 'Z') {
        *drive = c - 'A';
    }
}

/*---------------------------------------------------------------------------
 * parse_path_value - Copy a path value, stripping trailing CR/LF
 *---------------------------------------------------------------------------*/
static void parse_path_value(char *dst, const char *value)
{
    uint16_t len;

    str_copy_n(dst, value, MAX_PATH_LEN);
    len = str_len(dst);
    while (len > 0 && (dst[len-1] == '\n' || dst[len-1] == '\r')) {
        dst[--len] = '\0';
    }
}

/*---------------------------------------------------------------------------
 * parse_line - Parse a configuration line
 *---------------------------------------------------------------------------*/
static void parse_line(Config *cfg, const char *line)
{
    const char *key;
    const char *value;
    const char *p;
    uint16_t key_len;

    /* Skip whitespace */
    line = skip_whitespace(line);

    /* Skip comments and empty lines */
    if (*line == ';' || *line == '#' || *line == '\0' ||
        *line == '\n' || *line == '\r') {
        return;
    }

    /* Skip section headers */
    if (*line == '[') {
        return;
    }

    /* Find = sign */
    key = line;
    p = line;
    while (*p && *p != '=' && *p != '\n' && *p != '\r') p++;
    if (*p != '=') return;

    /* Key is everything before '=', minus trailing whitespace */
    key_len = (uint16_t)(p - key);
    while (key_len > 0 &&
           (key[key_len-1] == ' ' || key[key_len-1] == '\t')) {
        key_len--;
    }

    /* Get value */
    value = skip_whitespace(p + 1);

    /* Match known keys */
    if (key_is(key, key_len, "LeftDrive")) {
        parse_drive_value(&cfg->left_drive, value);
    }
    else if (key_is(key, key_len, "LeftPath")) {
        parse_path_value(cfg->left_path, value);
    }
    else if (key_is(key, key_len, "RightDrive")) {
        parse_drive_value(&cfg->right_drive, value);
    }
    else if (key_is(key, key_len, "RightPath")) {
        parse_path_value(cfg->right_path, value);
    }
    else if (key_is(key, key_len, "ActivePanel")) {
        if (*value == '1' || char_upper(*value) == 'R') {
            cfg->active_panel = 1;
        } else {
            cfg->active_panel = 0;
        }
    }
}

/*---------------------------------------------------------------------------
 * config_load - Load configuration from IGC.INI
 *---------------------------------------------------------------------------*/
bool_t config_load(Config *cfg)
{
    dos_handle_t h;
    char line[128];
    int16_t bytes;
    uint16_t line_pos = 0;
    uint16_t i;
    bool_t got_data;
    char buf[512];

    /* Set defaults */
    cfg->left_drive = dos_get_drive();
    cfg->left_path[0] = '\\';
    cfg->left_path[1] = '\0';
    cfg->right_drive = cfg->left_drive;
    cfg->right_path[0] = '\\';
    cfg->right_path[1] = '\0';
    cfg->active_panel = 0;

    /* Try to open config file */
    h = dos_open(config_path, DOS_OPEN_READ);
    if (h < 0) {
        return FALSE;
    }

    /* Read in chunks, splitting into lines as we go */
    got_data = FALSE;
    line_pos = 0;
    for (;;) {
        bytes = dos_read(h, buf, sizeof(buf));
        if (bytes <= 0) break;
        got_data = TRUE;

        for (i = 0; i < (uint16_t)bytes; i++) {
            if (buf[i] == '\n') {
                line[line_pos] = '\0';
                parse_line(cfg, line);
                line_pos = 0;
            } else if (buf[i] != '\r') {
                if (line_pos < sizeof(line) - 1) {
                    line[line_pos++] = buf[i];
                }
            }
        }
    }

    dos_close(h);

    if (!got_data) {
        return FALSE;
    }

    /* Last line may have no trailing newline */
    if (line_pos > 0) {
        line[line_pos] = '\0';
        parse_line(cfg, line);
    }

    return TRUE;
}

/*---------------------------------------------------------------------------
 * write_str - Write a string, returning FALSE on error or short write
 *---------------------------------------------------------------------------*/
static bool_t write_str(dos_handle_t h, const char *s)
{
    uint16_t len = str_len(s);

    return (dos_write(h, s, len) == (int16_t)len) ? TRUE : FALSE;
}

/*---------------------------------------------------------------------------
 * write_setting - Write "key<value>\r\n" where value is one character
 *---------------------------------------------------------------------------*/
static bool_t write_char_setting(dos_handle_t h, const char *key, char value)
{
    char buf[40];
    uint16_t len;

    str_copy(buf, key);
    len = str_len(buf);
    buf[len] = value;
    str_copy(&buf[len + 1], "\r\n");

    return write_str(h, buf);
}

/*---------------------------------------------------------------------------
 * write_path_setting - Write "key<path>\r\n"
 *---------------------------------------------------------------------------*/
static bool_t write_path_setting(dos_handle_t h, const char *key,
                                 const char *path)
{
    char buf[96];

    str_copy(buf, key);
    str_copy(buf + str_len(buf), path);
    str_copy(buf + str_len(buf), "\r\n");

    return write_str(h, buf);
}

/*---------------------------------------------------------------------------
 * config_save - Save configuration to IGC.INI
 *---------------------------------------------------------------------------*/
bool_t config_save(const Config *cfg)
{
    dos_handle_t h;
    bool_t ok;

    h = dos_create(config_path, 0);
    if (h < 0) {
        return FALSE;
    }

    ok = write_str(h, "; IGC Configuration\r\n[Settings]\r\n");
    ok = ok && write_char_setting(h, "LeftDrive=", 'A' + cfg->left_drive);
    ok = ok && write_path_setting(h, "LeftPath=", cfg->left_path);
    ok = ok && write_char_setting(h, "RightDrive=", 'A' + cfg->right_drive);
    ok = ok && write_path_setting(h, "RightPath=", cfg->right_path);
    ok = ok && write_char_setting(h, "ActivePanel=", '0' + cfg->active_panel);

    dos_close(h);
    return ok;
}

/*---------------------------------------------------------------------------
 * config_apply - Apply configuration to panels
 *---------------------------------------------------------------------------*/
void config_apply(const Config *cfg)
{
    /* Set left panel */
    g_left_panel.drive = cfg->left_drive;
    str_copy(g_left_panel.path, cfg->left_path);

    /* Set right panel */
    g_right_panel.drive = cfg->right_drive;
    str_copy(g_right_panel.path, cfg->right_path);

    /* Set active panel */
    g_active_panel = cfg->active_panel;
}

/*---------------------------------------------------------------------------
 * config_build - Build configuration from current state
 *---------------------------------------------------------------------------*/
void config_build(Config *cfg)
{
    /* A serial panel has no meaningful drive: persisting its drive 0 would
       restore as an empty floppy A: on the next launch (a "not ready" critical
       error). Save a safe local drive + root instead; the user re-selects
       Serial A by hand (we never auto-reconnect the serial link on load). */
    if (g_left_panel.backend == PANEL_SERIAL) {
        cfg->left_drive = dos_get_drive();
        cfg->left_path[0] = '\\';
        cfg->left_path[1] = '\0';
    } else {
        cfg->left_drive = g_left_panel.drive;
        str_copy(cfg->left_path, g_left_panel.path);
    }

    if (g_right_panel.backend == PANEL_SERIAL) {
        cfg->right_drive = dos_get_drive();
        cfg->right_path[0] = '\\';
        cfg->right_path[1] = '\0';
    } else {
        cfg->right_drive = g_right_panel.drive;
        str_copy(cfg->right_path, g_right_panel.path);
    }

    cfg->active_panel = g_active_panel;
}
