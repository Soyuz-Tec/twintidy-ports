/*
 * scanner.h — duplicate-detection engine shared by the CLI and the GUI.
 *
 * The engine is read-only: it never deletes, moves, or modifies a file.
 * It follows the same staged pipeline as TwinTidy's Go engine:
 *
 *   surface scan  -> inventory of user-created files, with category stats
 *   size mapping  -> only same-size files can be duplicates
 *   boundary hash -> cheap head+tail comparison rejects most candidates
 *   full hash     -> SHA-256 over the whole file
 *   confirmation  -> byte-for-byte compare, so a hash collision cannot
 *                    produce a false duplicate
 */

#ifndef TWINTIDY_SCANNER_H
#define TWINTIDY_SCANNER_H

#include <stddef.h>
#include <stdint.h>

#include "safety.h"
#include "sha256.h"

typedef struct {
    char *path;
    uint64_t size;
    int64_t modified_at; /* seconds since the Unix epoch */
    td_category category;
} td_file;

/* One confirmed duplicate group: files whose full contents matched. */
typedef struct {
    uint64_t size;
    char hash[TD_SHA256_HEX_SIZE];
    td_file *files;
    size_t count;
} td_group;

/* Per-category totals produced by the surface scan. */
typedef struct {
    size_t files;
    uint64_t bytes;
} td_category_stats;

/* Inventory of candidate files, produced before any hashing. */
typedef struct {
    td_file *files;
    size_t count;
    uint64_t total_bytes;
    size_t directories_scanned;
    size_t skipped_system_items;
    size_t errors_ignored;
    td_category_stats category_stats[TD_CATEGORY_COUNT];
} td_surface;

typedef struct {
    td_group *groups;
    size_t count;
    uint64_t reclaimable;
    size_t files_considered;
    size_t files_unreadable;
} td_result;

/* Caller-supplied scan policy. A zeroed struct means "no extra filtering". */
typedef struct {
    /* Include a category when categories[c] is non-zero. All-zero means all. */
    int categories[TD_CATEGORY_COUNT];
    uint64_t min_file_size;
    const char *const *excluded_paths;
    size_t excluded_path_count;
    const char *const *excluded_extensions;
    size_t excluded_extension_count;
    /* Cardinality caps; zero selects the production default. */
    size_t max_directories;
    size_t max_files;
} td_options;

/*
 * Progress callback. `total` is zero while the work size is unknown.
 * Return 0 to request cancellation, non-zero to continue.
 */
typedef int (*td_progress_fn)(void *ctx, const char *stage, size_t done, size_t total);

#define TD_OK          0
#define TD_ERR_MEMORY  1
#define TD_ERR_ROOT    2
#define TD_CANCELLED   3
#define TD_ERR_LIMIT   4

/* Production-safe defaults, mirroring the Go engine's bounds. */
td_options td_default_options(void);

/*
 * Phase one: inventory the user-created files under `root`. Protected
 * directories and protected extensions are excluded here, so later phases
 * never see them. The caller owns `*out` and must release it.
 */
int td_surface_scan(const char *root, const td_options *options, td_surface *out,
                    td_progress_fn progress, void *ctx);

/*
 * Phase two: find duplicates among an inventory, honouring the category
 * selection and filters in `options`. The surface report is not modified.
 */
int td_find_duplicates(const td_surface *surface, const td_options *options,
                       td_result *out, td_progress_fn progress, void *ctx);

/* Convenience: surface scan followed by duplicate detection. */
int td_scan(const char *root, const td_options *options, td_result *out,
            td_progress_fn progress, void *ctx);

void td_surface_free(td_surface *surface);
void td_result_free(td_result *result);

#endif /* TWINTIDY_SCANNER_H */
