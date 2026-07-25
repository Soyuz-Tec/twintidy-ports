/*
 * settings.c — persisted interface preferences. See settings.h.
 *
 * The format is a small line-oriented key=value file rather than JSON: it
 * is trivially parseable without a dependency, and a malformed line is
 * simply ignored instead of invalidating the whole file.
 */

#define _DEFAULT_SOURCE

#include "settings.h"

#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <stdlib.h>
#include <string.h>

td_settings td_settings_defaults(void) {
    td_settings value;
    memset(&value, 0, sizeof value);
    return value;
}

int td_settings_path(char *out, size_t size) {
    if (out == NULL || size == 0) {
        return 0;
    }
    const char *base = getenv("LOCALAPPDATA");
    if (base == NULL || *base == '\0') {
        base = getenv("HOME");
    }
    if (base == NULL || *base == '\0') {
        return 0;
    }
    int written = snprintf(out, size, "%s%cTwinTidyCPort", base,
#ifdef _WIN32
                           '\\'
#else
                           '/'
#endif
    );
    if (written < 0 || (size_t)written >= size) {
        return 0;
    }
#ifdef _WIN32
    _mkdir(out);
#else
    mkdir(out, 0700);
#endif
    written = snprintf(out + written, size - (size_t)written, "%csettings.ini",
#ifdef _WIN32
                       '\\'
#else
                       '/'
#endif
    );
    return written > 0;
}

td_settings td_settings_load(const char *path) {
    td_settings value = td_settings_defaults();
    if (path == NULL) {
        return value;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return value; /* fail open: no stored preferences yet */
    }

    char line[640];
    while (fgets(line, sizeof line, file) != NULL) {
        char *equals = strchr(line, '=');
        if (equals == NULL) {
            continue; /* ignore malformed lines rather than failing */
        }
        *equals = '\0';
        char *key = line;
        char *raw = equals + 1;
        size_t length = strlen(raw);
        while (length > 0 && (raw[length - 1] == '\n' || raw[length - 1] == '\r')) {
            raw[--length] = '\0';
        }

        if (strcmp(key, "x") == 0) {
            value.x = atoi(raw);
        } else if (strcmp(key, "y") == 0) {
            value.y = atoi(raw);
        } else if (strcmp(key, "width") == 0) {
            value.width = atoi(raw);
        } else if (strcmp(key, "height") == 0) {
            value.height = atoi(raw);
        } else if (strcmp(key, "maximized") == 0) {
            value.maximized = atoi(raw) != 0;
        } else if (strcmp(key, "lastFolder") == 0) {
            snprintf(value.last_folder, sizeof value.last_folder, "%s", raw);
        }
    }
    fclose(file);

    /* A stored placement with no extent is meaningless; discard it so the
     * caller falls back to its own default size. */
    if (value.width <= 0 || value.height <= 0) {
        value.x = value.y = value.width = value.height = 0;
    }
    return value;
}

int td_settings_save(const char *path, const td_settings *value) {
    if (path == NULL || value == NULL) {
        return 0;
    }
    size_t length = strlen(path) + 5;
    char *staging = malloc(length);
    if (staging == NULL) {
        return 0;
    }
    snprintf(staging, length, "%s.tmp", path);

    FILE *file = fopen(staging, "wb");
    if (file == NULL) {
        free(staging);
        return 0;
    }
    fprintf(file, "x=%d\n", value->x);
    fprintf(file, "y=%d\n", value->y);
    fprintf(file, "width=%d\n", value->width);
    fprintf(file, "height=%d\n", value->height);
    fprintf(file, "maximized=%d\n", value->maximized ? 1 : 0);
    fprintf(file, "lastFolder=%s\n", value->last_folder);

    int ok = ferror(file) == 0;
    if (fclose(file) != 0) {
        ok = 0;
    }
    if (ok) {
        remove(path); /* rename() will not replace on Windows */
        ok = rename(staging, path) == 0;
    }
    if (!ok) {
        remove(staging);
    }
    free(staging);
    return ok;
}
