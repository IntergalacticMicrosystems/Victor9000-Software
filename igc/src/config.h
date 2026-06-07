/*
 * config.h - IGC Configuration
 * Save/load settings from IGC.INI
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "igc.h"

/*---------------------------------------------------------------------------
 * Configuration Structure
 *---------------------------------------------------------------------------*/
typedef struct {
    uint8_t left_drive;             /* Left panel drive */
    char    left_path[MAX_PATH_LEN]; /* Left panel path */
    uint8_t right_drive;            /* Right panel drive */
    char    right_path[MAX_PATH_LEN]; /* Right panel path */
    uint8_t active_panel;           /* Active panel (0=left, 1=right) */
} Config;

/*---------------------------------------------------------------------------
 * Configuration Functions
 *---------------------------------------------------------------------------*/

/* Set the directory for IGC.INI based on the executable path (argv[0]).
 * Falls back to the current working directory if exe_path is empty/NULL. */
void config_init_path(const char *exe_path);

/* Load configuration from IGC.INI */
bool_t config_load(Config *cfg);

/* Save configuration to IGC.INI */
bool_t config_save(const Config *cfg);

/* Apply configuration to panels */
void config_apply(const Config *cfg);

/* Build configuration from current panel state */
void config_build(Config *cfg);

#endif /* CONFIG_H */
