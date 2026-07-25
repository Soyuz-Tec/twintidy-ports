/*
 * settings.h — interface preferences persisted between sessions.
 *
 * Loading is fail-open: a missing, unreadable, or malformed file yields
 * defaults rather than an error, so a corrupt preference file can never
 * prevent the application from starting.
 */

#ifndef TWINTIDY_SETTINGS_H
#define TWINTIDY_SETTINGS_H

#include <stddef.h>

typedef struct {
    /* Window placement in screen coordinates. Zero width or height means
     * "no stored placement"; the caller should use its own default. */
    int x, y, width, height;
    int maximized;
    /* Last folder the user scanned, empty when none. */
    char last_folder[512];
} td_settings;

/* Defaults used when nothing valid is stored. */
td_settings td_settings_defaults(void);

/*
 * Resolve the settings file path into `out`. Returns non-zero on success.
 * The file lives beside the user's local application data.
 */
int td_settings_path(char *out, size_t size);

/* Load settings, returning defaults on any failure. */
td_settings td_settings_load(const char *path);

/*
 * Save settings atomically: written to a staging file and renamed into
 * place, so an interrupted write cannot corrupt the stored preferences.
 * Returns non-zero on success.
 */
int td_settings_save(const char *path, const td_settings *value);

#endif /* TWINTIDY_SETTINGS_H */
