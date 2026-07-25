/*
 * scanner.c — staged duplicate detection. See scanner.h for the pipeline.
 * Read-only throughout; nothing here writes to the scanned tree.
 */

/* Expose POSIX declarations (strdup, lstat) under strict -std=c11 on
 * glibc; without this they are implicitly declared and the returned
 * pointer from strdup is truncated to int, crashing on 64-bit Linux. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "scanner.h"

#include <ctype.h>
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

#define BOUNDARY_READ_SIZE 4096
#define HASH_BUFFER_SIZE   65536
#define DEFAULT_MAX_DIRECTORIES 250000
#define DEFAULT_MAX_FILES       500000

typedef struct {
    td_progress_fn fn;
    void *ctx;
    int cancelled;
} progress_sink;

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

td_options td_default_options(void) {
    td_options options;
    memset(&options, 0, sizeof options);
    options.max_directories = DEFAULT_MAX_DIRECTORIES;
    options.max_files = DEFAULT_MAX_FILES;
    return options;
}

static size_t option_max_directories(const td_options *options) {
    if (options == NULL || options->max_directories == 0) {
        return DEFAULT_MAX_DIRECTORIES;
    }
    return options->max_directories;
}

static size_t option_max_files(const td_options *options) {
    if (options == NULL || options->max_files == 0) {
        return DEFAULT_MAX_FILES;
    }
    return options->max_files;
}

/* ---------- small helpers ---------- */

static int equals_fold(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == *right;
}

static const char *extension_of(const char *path) {
    const char *last_dot = NULL;
    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
            last_dot = NULL;
        } else if (*cursor == '.') {
            last_dot = cursor;
        }
    }
    return last_dot != NULL ? last_dot : "";
}

/*
 * Match on whole path-segment boundaries and case-insensitively, so an
 * exclusion for C:\Data never captures C:\Database.
 */
static int path_within(const char *path, const char *prefix) {
    if (prefix == NULL || *prefix == '\0') {
        return 0;
    }
    size_t prefix_length = strlen(prefix);
    while (prefix_length > 0 &&
           (prefix[prefix_length - 1] == '/' || prefix[prefix_length - 1] == '\\')) {
        prefix_length--;
    }
    if (strlen(path) < prefix_length) {
        return 0;
    }
    for (size_t i = 0; i < prefix_length; i++) {
        char left = (char)tolower((unsigned char)path[i]);
        char right = (char)tolower((unsigned char)prefix[i]);
        if (left == '/') {
            left = '\\';
        }
        if (right == '/') {
            right = '\\';
        }
        if (left != right) {
            return 0;
        }
    }
    char next = path[prefix_length];
    return next == '\0' || next == '/' || next == '\\';
}

static int file_passes_filters(const td_file *file, const td_options *options) {
    if (options == NULL) {
        return 1;
    }
    if (options->min_file_size > 0 && file->size < options->min_file_size) {
        return 0;
    }
    const char *extension = extension_of(file->path);
    for (size_t i = 0; i < options->excluded_extension_count; i++) {
        const char *excluded = options->excluded_extensions[i];
        if (excluded == NULL || *excluded == '\0') {
            continue;
        }
        if (*excluded != '.') {
            /* Accept "tmp" as well as ".tmp". */
            size_t length = strlen(excluded);
            if (*extension == '.' && equals_fold(extension + 1, excluded) &&
                strlen(extension + 1) == length) {
                return 0;
            }
            continue;
        }
        if (equals_fold(extension, excluded)) {
            return 0;
        }
    }
    for (size_t i = 0; i < options->excluded_path_count; i++) {
        if (path_within(file->path, options->excluded_paths[i])) {
            return 0;
        }
    }
    return 1;
}

static int category_selected(const td_options *options, td_category category) {
    if (options == NULL) {
        return 1;
    }
    int any = 0;
    for (size_t i = 0; i < TD_CATEGORY_COUNT; i++) {
        if (options->categories[i]) {
            any = 1;
            break;
        }
    }
    if (!any) {
        return 1; /* no explicit selection means every category */
    }
    return options->categories[category] != 0;
}

/* ---------- surface scan ---------- */

typedef struct {
    td_file *items;
    size_t count;
    size_t capacity;
} inventory;

static int inventory_add(inventory *inv, const char *path, uint64_t size,
                         int64_t modified_at, td_category category) {
    if (inv->count == inv->capacity) {
        size_t capacity = inv->capacity ? inv->capacity * 2 : 256;
        td_file *grown = realloc(inv->items, capacity * sizeof *grown);
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
    inv->items[inv->count].modified_at = modified_at;
    inv->items[inv->count].category = category;
    inv->count++;
    return 1;
}

/*
 * Directories still to visit. Traversal uses this explicit stack rather than
 * recursion: a deeply nested tree — which a scan target may legitimately be,
 * and which a hostile one certainly can be — would otherwise exhaust the
 * call stack before any cardinality limit was reached.
 */
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} pending_dirs;

static int pending_push(pending_dirs *pending, const char *path) {
    if (pending->count == pending->capacity) {
        size_t capacity = pending->capacity ? pending->capacity * 2 : 64;
        char **grown = realloc(pending->items, capacity * sizeof *grown);
        if (grown == NULL) {
            return 0;
        }
        pending->items = grown;
        pending->capacity = capacity;
    }
    char *copy = strdup(path);
    if (copy == NULL) {
        return 0;
    }
    pending->items[pending->count++] = copy;
    return 1;
}

static char *pending_pop(pending_dirs *pending) {
    if (pending->count == 0) {
        return NULL;
    }
    return pending->items[--pending->count];
}

static void pending_free(pending_dirs *pending) {
    for (size_t i = 0; i < pending->count; i++) {
        free(pending->items[i]);
    }
    free(pending->items);
    pending->items = NULL;
    pending->count = 0;
    pending->capacity = 0;
}

/* Read one directory, queueing any subdirectories onto `pending`. */
static int read_directory(const char *dir, inventory *inv, td_surface *report_out,
                          const td_options *options, progress_sink *sink,
                          pending_dirs *pending) {
    if (report_out->directories_scanned >= option_max_directories(options)) {
        return TD_ERR_LIMIT;
    }
    DIR *handle = opendir(dir);
    if (handle == NULL) {
        report_out->errors_ignored++;
        return TD_OK;
    }
    report_out->directories_scanned++;

    struct dirent *item;
    int status = TD_OK;
    while ((item = readdir(handle)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (!report(sink, "Surface scan", inv->count, 0)) {
            status = TD_CANCELLED;
            break;
        }

        size_t length = strlen(dir) + 1 + strlen(item->d_name) + 1;
        char *path = malloc(length);
        if (path == NULL) {
            status = TD_ERR_MEMORY;
            break;
        }
        snprintf(path, length, "%s%c%s", dir, PATH_SEP, item->d_name);

        struct stat info;
#ifdef _WIN32
        if (stat(path, &info) != 0) {
            report_out->errors_ignored++;
            free(path);
            continue;
        }
#else
        if (lstat(path, &info) != 0) {
            report_out->errors_ignored++;
            free(path);
            continue;
        }
        if (S_ISLNK(info.st_mode)) {
            /* Symlinks are never followed: they can escape the scan root and
             * create traversal cycles. */
            report_out->skipped_system_items++;
            free(path);
            continue;
        }
#endif
        if (S_ISDIR(info.st_mode)) {
            if (td_should_skip_directory(path)) {
                report_out->skipped_system_items++;
            } else if (!pending_push(pending, path)) {
                status = TD_ERR_MEMORY;
            }
        } else if (S_ISREG(info.st_mode)) {
            if (!td_is_user_created_file(path)) {
                report_out->skipped_system_items++;
            } else if (inv->count >= option_max_files(options)) {
                status = TD_ERR_LIMIT;
            } else {
                td_category category = td_category_for_path(path);
                if (!inventory_add(inv, path, (uint64_t)info.st_size,
                                   (int64_t)info.st_mtime, category)) {
                    status = TD_ERR_MEMORY;
                } else {
                    report_out->total_bytes += (uint64_t)info.st_size;
                    report_out->category_stats[category].files++;
                    report_out->category_stats[category].bytes += (uint64_t)info.st_size;
                }
            }
        }
        free(path);
        if (status != TD_OK) {
            break;
        }
    }
    closedir(handle);
    return status;
}

/* Traverse `root` iteratively, draining the pending-directory stack. */
static int walk(const char *root, inventory *inv, td_surface *report_out,
                const td_options *options, progress_sink *sink) {
    pending_dirs pending = {NULL, 0, 0};
    if (!pending_push(&pending, root)) {
        return TD_ERR_MEMORY;
    }

    int status = TD_OK;
    char *dir;
    while ((dir = pending_pop(&pending)) != NULL) {
        status = read_directory(dir, inv, report_out, options, sink, &pending);
        free(dir);
        if (status != TD_OK) {
            break;
        }
    }
    pending_free(&pending);
    return status;
}

int td_surface_scan(const char *root, const td_options *options, td_surface *out,
                    td_progress_fn progress, void *ctx) {
    if (root == NULL || out == NULL) {
        return TD_ERR_ROOT;
    }
    memset(out, 0, sizeof *out);
    progress_sink sink = {progress, ctx, 0};

    if (td_should_skip_directory(root)) {
        /* The user explicitly chose this root, but it is a protected system
         * or build location; refuse rather than silently returning nothing. */
        return TD_ERR_ROOT;
    }

    inventory inv = {NULL, 0, 0};
    int status = walk(root, &inv, out, options, &sink);
    if (status != TD_OK) {
        for (size_t i = 0; i < inv.count; i++) {
            free(inv.items[i].path);
        }
        free(inv.items);
        return status;
    }

    out->files = inv.items;
    out->count = inv.count;
    report(&sink, "Surface scan complete", inv.count, inv.count);
    return TD_OK;
}

/* ---------- hashing ---------- */

/* FNV-1a over the head and tail of a file: a cheap pre-filter that rejects
 * most same-size candidates without reading whole files. */
static int boundary_hash(const char *path, uint64_t size, uint64_t *out) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    unsigned char head[BOUNDARY_READ_SIZE];
    unsigned char tail[BOUNDARY_READ_SIZE];
    size_t want = size < BOUNDARY_READ_SIZE ? (size_t)size : BOUNDARY_READ_SIZE;

    size_t head_read = fread(head, 1, want, file);
    if (head_read != want) {
        fclose(file);
        return 0;
    }
    if (size > BOUNDARY_READ_SIZE) {
        if (fseek(file, (long)(size - want), SEEK_SET) != 0) {
            fclose(file);
            return 0;
        }
    } else {
        rewind(file);
    }
    size_t tail_read = fread(tail, 1, want, file);
    int failed = ferror(file);
    fclose(file);
    if (tail_read != want || failed) {
        return 0;
    }

    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < 8; i++) {
        hash ^= (unsigned char)(size >> (i * 8));
        hash *= 1099511628211ULL;
    }
    for (size_t i = 0; i < want; i++) {
        hash ^= head[i];
        hash *= 1099511628211ULL;
    }
    for (size_t i = 0; i < want; i++) {
        hash ^= tail[i];
        hash *= 1099511628211ULL;
    }
    *out = hash;
    return 1;
}

static int full_hash(const char *path, char out[TD_SHA256_HEX_SIZE]) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    td_sha256 context;
    td_sha256_init(&context);
    unsigned char buffer[HASH_BUFFER_SIZE];
    size_t read;
    while ((read = fread(buffer, 1, sizeof buffer, file)) > 0) {
        td_sha256_update(&context, buffer, read);
    }
    int failed = ferror(file);
    fclose(file);
    if (failed) {
        return 0;
    }
    unsigned char digest[TD_SHA256_DIGEST_SIZE];
    td_sha256_final(&context, digest);
    td_sha256_hex(digest, out);
    return 1;
}

/* Byte-for-byte comparison: the final arbiter, immune to hash collisions. */
static int same_bytes(const char *left_path, const char *right_path) {
    FILE *left = fopen(left_path, "rb");
    FILE *right = fopen(right_path, "rb");
    int same = 0;
    if (left != NULL && right != NULL) {
        unsigned char left_buffer[HASH_BUFFER_SIZE];
        unsigned char right_buffer[HASH_BUFFER_SIZE];
        size_t left_read, right_read;
        same = 1;
        do {
            left_read = fread(left_buffer, 1, sizeof left_buffer, left);
            right_read = fread(right_buffer, 1, sizeof right_buffer, right);
            if (left_read != right_read ||
                memcmp(left_buffer, right_buffer, left_read) != 0) {
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

/* ---------- duplicate detection ---------- */

typedef struct {
    const td_file *file;
    uint64_t boundary;
    char hash[TD_SHA256_HEX_SIZE];
    int hashed;
    int claimed;
} candidate;

static int compare_candidates(const void *left, const void *right) {
    const candidate *a = left;
    const candidate *b = right;
    if (a->file->size != b->file->size) {
        return a->file->size < b->file->size ? -1 : 1;
    }
    return strcmp(a->file->path, b->file->path);
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

static int copy_group_files(td_group *group, candidate *candidates,
                            const size_t *members, size_t member_count) {
    group->files = calloc(member_count, sizeof *group->files);
    if (group->files == NULL) {
        return 0;
    }
    for (size_t m = 0; m < member_count; m++) {
        const td_file *source = candidates[members[m]].file;
        group->files[m].size = source->size;
        group->files[m].modified_at = source->modified_at;
        group->files[m].category = source->category;
        group->files[m].path = strdup(source->path);
        if (group->files[m].path == NULL) {
            for (size_t undo = 0; undo < m; undo++) {
                free(group->files[undo].path);
            }
            free(group->files);
            group->files = NULL;
            return 0;
        }
    }
    group->count = member_count;
    return 1;
}

int td_find_duplicates(const td_surface *surface, const td_options *options,
                       td_result *out, td_progress_fn progress, void *ctx) {
    if (surface == NULL || out == NULL) {
        return TD_ERR_ROOT;
    }
    memset(out, 0, sizeof *out);
    progress_sink sink = {progress, ctx, 0};

    candidate *candidates = calloc(surface->count ? surface->count : 1,
                                   sizeof *candidates);
    if (candidates == NULL) {
        return TD_ERR_MEMORY;
    }
    size_t candidate_count = 0;
    for (size_t i = 0; i < surface->count; i++) {
        const td_file *file = &surface->files[i];
        if (!category_selected(options, file->category)) {
            continue;
        }
        if (!file_passes_filters(file, options)) {
            continue;
        }
        candidates[candidate_count].file = file;
        candidate_count++;
    }
    out->files_considered = candidate_count;
    if (candidate_count == 0) {
        free(candidates);
        report(&sink, "Done", 0, 0);
        return TD_OK;
    }

    qsort(candidates, candidate_count, sizeof *candidates, compare_candidates);

    size_t capacity = 0;
    size_t processed = 0;
    int status = TD_OK;

    for (size_t start = 0; start < candidate_count && status == TD_OK;) {
        size_t end = start + 1;
        while (end < candidate_count &&
               candidates[end].file->size == candidates[start].file->size) {
            end++;
        }

        /* Only same-size files can match; empty files are ignored. */
        if (end - start < 2 || candidates[start].file->size == 0) {
            processed += end - start;
            start = end;
            continue;
        }

        /* Stage: boundary hashing rejects most candidates cheaply. */
        for (size_t i = start; i < end; i++) {
            if (!report(&sink, "Boundary hashing", processed, candidate_count)) {
                status = TD_CANCELLED;
                break;
            }
            processed++;
            if (!boundary_hash(candidates[i].file->path, candidates[i].file->size,
                               &candidates[i].boundary)) {
                candidates[i].claimed = 1; /* unreadable: never a candidate */
                out->files_unreadable++;
            }
        }
        if (status != TD_OK) {
            break;
        }

        /* Stage: full SHA-256 only for files sharing a boundary hash. */
        for (size_t i = start; i < end; i++) {
            if (candidates[i].claimed) {
                continue;
            }
            int shares_boundary = 0;
            for (size_t j = start; j < end; j++) {
                if (j != i && !candidates[j].claimed &&
                    candidates[j].boundary == candidates[i].boundary) {
                    shares_boundary = 1;
                    break;
                }
            }
            if (!shares_boundary) {
                continue;
            }
            if (!report(&sink, "Full hashing", processed, candidate_count)) {
                status = TD_CANCELLED;
                break;
            }
            if (full_hash(candidates[i].file->path, candidates[i].hash)) {
                candidates[i].hashed = 1;
            } else {
                candidates[i].claimed = 1;
                out->files_unreadable++;
            }
        }
        if (status != TD_OK) {
            break;
        }

        /* Stage: confirm byte-for-byte, then record the group. */
        for (size_t i = start; i < end && status == TD_OK; i++) {
            if (!candidates[i].hashed || candidates[i].claimed) {
                continue;
            }
            /* Sized to the whole size group. A fixed cap would silently split
             * one large duplicate group into several, which misreports the
             * relationship and skews the reclaimable estimate by one file for
             * every spurious extra group. */
            size_t *members = calloc(end - start, sizeof *members);
            if (members == NULL) {
                status = TD_ERR_MEMORY;
                break;
            }
            size_t member_count = 0;
            members[member_count++] = i;

            for (size_t j = i + 1; j < end; j++) {
                if (!candidates[j].hashed || candidates[j].claimed) {
                    continue;
                }
                if (strcmp(candidates[j].hash, candidates[i].hash) != 0) {
                    continue;
                }
                if (same_bytes(candidates[i].file->path, candidates[j].file->path)) {
                    candidates[j].claimed = 1;
                    members[member_count++] = j;
                }
            }

            if (member_count >= 2) {
                td_group group;
                memset(&group, 0, sizeof group);
                group.size = candidates[i].file->size;
                memcpy(group.hash, candidates[i].hash, TD_SHA256_HEX_SIZE);
                if (!copy_group_files(&group, candidates, members, member_count) ||
                    !result_add_group(out, &capacity, group)) {
                    for (size_t m = 0; m < group.count; m++) {
                        free(group.files[m].path);
                    }
                    free(group.files);
                    free(members);
                    status = TD_ERR_MEMORY;
                    break;
                }
                out->reclaimable += group.size * (uint64_t)(member_count - 1);
            }
            free(members);
        }
        start = end;
    }

    free(candidates);
    if (status != TD_OK) {
        td_result_free(out);
        return status;
    }
    report(&sink, "Done", candidate_count, candidate_count);
    return TD_OK;
}

int td_scan(const char *root, const td_options *options, td_result *out,
            td_progress_fn progress, void *ctx) {
    td_surface surface;
    int status = td_surface_scan(root, options, &surface, progress, ctx);
    if (status != TD_OK) {
        return status;
    }
    status = td_find_duplicates(&surface, options, out, progress, ctx);
    td_surface_free(&surface);
    return status;
}

void td_surface_free(td_surface *surface) {
    if (surface == NULL) {
        return;
    }
    for (size_t i = 0; i < surface->count; i++) {
        free(surface->files[i].path);
    }
    free(surface->files);
    surface->files = NULL;
    surface->count = 0;
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
