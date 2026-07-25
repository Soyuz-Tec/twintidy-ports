/*
 * scanner.c — duplicate detection: walk, group by size, hash, then confirm
 * byte-for-byte. Read-only throughout; nothing here writes to the scanned
 * tree. See scanner.h for the contract.
 */

/* Expose POSIX declarations (strdup, lstat) under strict -std=c11 on
 * glibc; without this they are implicitly declared and the returned
 * pointer from strdup is truncated to int, crashing on 64-bit Linux. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "scanner.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

typedef struct {
    char *path;
    uint64_t size;
    uint64_t hash;
    int hashed;
    int claimed;
} entry;

typedef struct {
    entry *items;
    size_t count;
    size_t capacity;
} inventory;

typedef struct {
    td_progress_fn fn;
    void *ctx;
    int cancelled;
} progress_sink;

/* report returns 0 when the caller asked to cancel. */
static int report(progress_sink *sink, const char *stage, size_t done, size_t total) {
    if (sink->cancelled) {
        return 0;
    }
    if (sink->fn == NULL) {
        return 1;
    }
    if (!sink->fn(sink->ctx, stage, done, total)) {
        sink->cancelled = 1;
        return 0;
    }
    return 1;
}

static int inventory_add(inventory *inv, const char *path, uint64_t size) {
    if (inv->count == inv->capacity) {
        size_t capacity = inv->capacity ? inv->capacity * 2 : 256;
        entry *grown = realloc(inv->items, capacity * sizeof *grown);
        if (grown == NULL) {
            return 0;
        }
        inv->items = grown;
        inv->capacity = capacity;
    }
    char *copy = strdup(path);
    if (copy == NULL) {
        return 0;
    }
    inv->items[inv->count].path = copy;
    inv->items[inv->count].size = size;
    inv->items[inv->count].hash = 0;
    inv->items[inv->count].hashed = 0;
    inv->items[inv->count].claimed = 0;
    inv->count++;
    return 1;
}

static void inventory_free(inventory *inv) {
    for (size_t i = 0; i < inv->count; i++) {
        free(inv->items[i].path);
    }
    free(inv->items);
    inv->items = NULL;
    inv->count = 0;
    inv->capacity = 0;
}

/*
 * walk recurses into dir. Symlinks are skipped where lstat is available so
 * traversal cycles are impossible. Unreadable directories are counted and
 * skipped rather than aborting the whole scan.
 */
static int walk(const char *dir, inventory *inv, progress_sink *sink, size_t *unreadable) {
    DIR *handle = opendir(dir);
    if (handle == NULL) {
        (*unreadable)++;
        return 1;
    }

    struct dirent *item;
    int ok = 1;
    while ((item = readdir(handle)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (!report(sink, "Scanning folders", inv->count, 0)) {
            ok = 0;
            break;
        }

        size_t length = strlen(dir) + 1 + strlen(item->d_name) + 1;
        char *path = malloc(length);
        if (path == NULL) {
            ok = 0;
            break;
        }
        snprintf(path, length, "%s%c%s", dir, PATH_SEP, item->d_name);

        struct stat info;
#ifdef _WIN32
        if (stat(path, &info) != 0) {
            (*unreadable)++;
            free(path);
            continue;
        }
#else
        if (lstat(path, &info) != 0) {
            (*unreadable)++;
            free(path);
            continue;
        }
        if (S_ISLNK(info.st_mode)) {
            free(path);
            continue;
        }
#endif
        if (S_ISDIR(info.st_mode)) {
            ok = walk(path, inv, sink, unreadable);
        } else if (S_ISREG(info.st_mode)) {
            if (!inventory_add(inv, path, (uint64_t)info.st_size)) {
                ok = 0;
            }
        }
        free(path);
        if (!ok) {
            break;
        }
    }
    closedir(handle);
    return ok;
}

/* Streamed FNV-1a 64-bit hash. *ok is cleared on any I/O error so callers
 * skip the file instead of trusting a partial digest. */
static uint64_t hash_file(const char *path, int *ok) {
    *ok = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    uint64_t hash = 1469598103934665603ULL;
    unsigned char buffer[1 << 16];
    size_t read;
    while ((read = fread(buffer, 1, sizeof buffer, file)) > 0) {
        for (size_t i = 0; i < read; i++) {
            hash ^= buffer[i];
            hash *= 1099511628211ULL;
        }
    }
    int failed = ferror(file);
    fclose(file);
    if (failed) {
        return 0;
    }
    *ok = 1;
    return hash;
}

/* Byte-for-byte comparison: the final arbiter, immune to hash collisions. */
static int same_bytes(const char *left_path, const char *right_path) {
    FILE *left = fopen(left_path, "rb");
    FILE *right = fopen(right_path, "rb");
    int same = 0;
    if (left != NULL && right != NULL) {
        unsigned char left_buffer[1 << 16];
        unsigned char right_buffer[1 << 16];
        size_t left_read, right_read;
        same = 1;
        do {
            left_read = fread(left_buffer, 1, sizeof left_buffer, left);
            right_read = fread(right_buffer, 1, sizeof right_buffer, right);
            if (left_read != right_read || memcmp(left_buffer, right_buffer, left_read) != 0) {
                same = 0;
                break;
            }
        } while (left_read > 0);
        if (ferror(left) || ferror(right)) {
            same = 0;
        }
    }
    if (left != NULL) {
        fclose(left);
    }
    if (right != NULL) {
        fclose(right);
    }
    return same;
}

static int compare_by_size(const void *left, const void *right) {
    const entry *a = left;
    const entry *b = right;
    if (a->size != b->size) {
        return a->size < b->size ? -1 : 1;
    }
    return strcmp(a->path, b->path);
}

static int result_add_group(td_result *result, size_t *capacity, td_group group) {
    if (result->count == *capacity) {
        size_t grown_capacity = *capacity ? *capacity * 2 : 32;
        td_group *grown = realloc(result->groups, grown_capacity * sizeof *grown);
        if (grown == NULL) {
            return 0;
        }
        result->groups = grown;
        *capacity = grown_capacity;
    }
    result->groups[result->count++] = group;
    return 1;
}

int td_scan(const char *root, td_result *out, td_progress_fn progress, void *ctx) {
    if (root == NULL || out == NULL) {
        return TD_ERR_ROOT;
    }
    memset(out, 0, sizeof *out);

    progress_sink sink = {progress, ctx, 0};
    inventory inv = {NULL, 0, 0};
    size_t unreadable = 0;

    if (!walk(root, &inv, &sink, &unreadable)) {
        inventory_free(&inv);
        return sink.cancelled ? TD_CANCELLED : TD_ERR_MEMORY;
    }
    if (inv.count == 0) {
        inventory_free(&inv);
        out->files_unreadable = unreadable;
        return TD_OK;
    }

    qsort(inv.items, inv.count, sizeof *inv.items, compare_by_size);

    size_t capacity = 0;
    size_t processed = 0;
    int status = TD_OK;

    for (size_t start = 0; start < inv.count && status == TD_OK;) {
        size_t end = start + 1;
        while (end < inv.count && inv.items[end].size == inv.items[start].size) {
            end++;
        }

        /* Only same-size files can be duplicates; empty files are ignored. */
        if (end - start >= 2 && inv.items[start].size > 0) {
            for (size_t i = start; i < end; i++) {
                if (!report(&sink, "Comparing files", processed, inv.count)) {
                    status = TD_CANCELLED;
                    break;
                }
                int ok;
                inv.items[i].hash = hash_file(inv.items[i].path, &ok);
                inv.items[i].hashed = ok;
                if (!ok) {
                    unreadable++;
                }
                processed++;
            }

            for (size_t i = start; i < end && status == TD_OK; i++) {
                if (!inv.items[i].hashed || inv.items[i].claimed) {
                    continue;
                }
                size_t members[64];
                size_t member_count = 0;
                members[member_count++] = i;

                for (size_t j = i + 1; j < end && member_count < 64; j++) {
                    if (!inv.items[j].hashed || inv.items[j].claimed) {
                        continue;
                    }
                    if (inv.items[j].hash != inv.items[i].hash) {
                        continue;
                    }
                    if (same_bytes(inv.items[i].path, inv.items[j].path)) {
                        inv.items[j].claimed = 1;
                        members[member_count++] = j;
                    }
                }

                if (member_count >= 2) {
                    td_group group;
                    group.size = inv.items[i].size;
                    group.count = member_count;
                    group.files = calloc(member_count, sizeof *group.files);
                    if (group.files == NULL) {
                        status = TD_ERR_MEMORY;
                        break;
                    }
                    int copied = 1;
                    for (size_t m = 0; m < member_count; m++) {
                        group.files[m].size = inv.items[members[m]].size;
                        group.files[m].path = strdup(inv.items[members[m]].path);
                        if (group.files[m].path == NULL) {
                            copied = 0;
                            break;
                        }
                    }
                    if (!copied || !result_add_group(out, &capacity, group)) {
                        for (size_t m = 0; m < member_count; m++) {
                            free(group.files[m].path);
                        }
                        free(group.files);
                        status = TD_ERR_MEMORY;
                        break;
                    }
                    out->reclaimable += group.size * (uint64_t)(member_count - 1);
                }
            }
        } else {
            processed += end - start;
        }
        start = end;
    }

    out->files_scanned = inv.count;
    out->files_unreadable = unreadable;
    inventory_free(&inv);

    if (status != TD_OK) {
        td_result_free(out);
        return status;
    }
    report(&sink, "Done", inv.count, inv.count);
    return TD_OK;
}

void td_result_free(td_result *result) {
    if (result == NULL) {
        return;
    }
    for (size_t g = 0; g < result->count; g++) {
        for (size_t f = 0; f < result->groups[g].count; f++) {
            free(result->groups[g].files[f].path);
        }
        free(result->groups[g].files);
    }
    free(result->groups);
    result->groups = NULL;
    result->count = 0;
    result->reclaimable = 0;
}
