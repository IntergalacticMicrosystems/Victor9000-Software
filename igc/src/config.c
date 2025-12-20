/*
 * config.c - IGC Configuration Implementation
 */

#include "config.h"
#include "panel.h"
#include "dosapi.h"
#include "util.h"

/*---------------------------------------------------------------------------
 * Configuration file name
 *---------------------------------------------------------------------------*/
static const char *CONFIG_FILE = "IGC.INI";

/*---------------------------------------------------------------------------
 * skip_whitespace - Skip whitespace in string
 *---------------------------------------------------------------------------*/
static const char *skip_whitespace(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/*---------------------------------------------------------------------------
 * parse_line - Parse a configuration line
 *---------------------------------------------------------------------------*/
static void parse_line(Config *cfg, const char *line)
{
    const char *key;
    const char *value;
    const char *p;

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

    /* Get value */
    value = skip_whitespace(p + 1);

    /* Match known keys */
    if (str_cmp_i(key, "LeftDrive") == 0 || line[0] == 'L') {
        /* Parse drive letter */
        if (*value >= 'A' && *value <= 'Z') {
            cfg->left_drive = *value - 'A';
        } else if (*value >= 'a' && *value <= 'z') {
            cfg->left_drive = *value - 'a';
        }
    }
    else if (str_cmp_i(key, "LeftPath") == 0 ||
             (line[0] == 'L' && line[4] == 'P')) {
        /* Copy path (strip trailing newline) */
        str_copy_n(cfg->left_path, value, MAX_PATH_LEN - 1);
        {
            uint16_t len = str_len(cfg->left_path);
            while (len > 0 &&
                   (cfg->left_path[len-1] == '\n' ||
                    cfg->left_path[len-1] == '\r')) {
                cfg->left_path[--len] = '\0';
            }
        }
    }
    else if (str_cmp_i(key, "RightDrive") == 0 || line[0] == 'R') {
        if (*value >= 'A' && *value <= 'Z') {
            cfg->right_drive = *value - 'A';
        } else if (*value >= 'a' && *value <= 'z') {
            cfg->right_drive = *value - 'a';
        }
    }
    else if (str_cmp_i(key, "RightPath") == 0 ||
             (line[0] == 'R' && line[5] == 'P')) {
        str_copy_n(cfg->right_path, value, MAX_PATH_LEN - 1);
        {
            uint16_t len = str_len(cfg->right_path);
            while (len > 0 &&
                   (cfg->right_path[len-1] == '\n' ||
                    cfg->right_path[len-1] == '\r')) {
                cfg->right_path[--len] = '\0';
            }
        }
    }
    else if (str_cmp_i(key, "ActivePanel") == 0 || line[0] == 'A') {
        if (*value == '1' || *value == 'R' || *value == 'r') {
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
    h = dos_open(CONFIG_FILE, DOS_OPEN_READ);
    if (h < 0) {
        return FALSE;
    }

    /* Read file in one chunk */
    bytes = dos_read(h, buf, sizeof(buf) - 1);
    dos_close(h);

    if (bytes <= 0) {
        return FALSE;
    }
    buf[bytes] = '\0';

    /* Parse line by line */
    line_pos = 0;
    for (i = 0; i <= bytes; i++) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            line[line_pos] = '\0';
            parse_line(cfg, line);
            line_pos = 0;
        } else if (buf[i] != '\r') {
            if (line_pos < sizeof(line) - 1) {
                line[line_pos++] = buf[i];
            }
        }
    }

    return TRUE;
}

/*---------------------------------------------------------------------------
 * config_save - Save configuration to IGC.INI
 *---------------------------------------------------------------------------*/
bool_t config_save(const Config *cfg)
{
    dos_handle_t h;
    char buf[256];
    uint16_t len;
    int16_t written;

    h = dos_create(CONFIG_FILE, 0);
    if (h < 0) {
        return FALSE;
    }

    /* Write header */
    str_copy(buf, "; IGC Configuration\r\n[Settings]\r\n");
    len = str_len(buf);
    written = dos_write(h, buf, len);
    if (written != len) {
        dos_close(h);
        return FALSE;
    }

    /* Left panel */
    buf[0] = 'L';
    buf[1] = 'e';
    buf[2] = 'f';
    buf[3] = 't';
    buf[4] = 'D';
    buf[5] = 'r';
    buf[6] = 'i';
    buf[7] = 'v';
    buf[8] = 'e';
    buf[9] = '=';
    buf[10] = 'A' + cfg->left_drive;
    buf[11] = '\r';
    buf[12] = '\n';
    buf[13] = '\0';
    len = str_len(buf);
    dos_write(h, buf, len);

    str_copy(buf, "LeftPath=");
    str_copy(buf + str_len(buf), cfg->left_path);
    str_copy(buf + str_len(buf), "\r\n");
    len = str_len(buf);
    dos_write(h, buf, len);

    /* Right panel */
    buf[0] = 'R';
    buf[1] = 'i';
    buf[2] = 'g';
    buf[3] = 'h';
    buf[4] = 't';
    buf[5] = 'D';
    buf[6] = 'r';
    buf[7] = 'i';
    buf[8] = 'v';
    buf[9] = 'e';
    buf[10] = '=';
    buf[11] = 'A' + cfg->right_drive;
    buf[12] = '\r';
    buf[13] = '\n';
    buf[14] = '\0';
    len = str_len(buf);
    dos_write(h, buf, len);

    str_copy(buf, "RightPath=");
    str_copy(buf + str_len(buf), cfg->right_path);
    str_copy(buf + str_len(buf), "\r\n");
    len = str_len(buf);
    dos_write(h, buf, len);

    /* Active panel */
    str_copy(buf, "ActivePanel=");
    buf[str_len(buf)] = '0' + cfg->active_panel;
    buf[str_len(buf) + 1] = '\0';
    str_copy(buf + str_len(buf), "\r\n");
    len = str_len(buf);
    dos_write(h, buf, len);

    dos_close(h);
    return TRUE;
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
    cfg->left_drive = g_left_panel.drive;
    str_copy(cfg->left_path, g_left_panel.path);
    cfg->right_drive = g_right_panel.drive;
    str_copy(cfg->right_path, g_right_panel.path);
    cfg->active_panel = g_active_panel;
}
