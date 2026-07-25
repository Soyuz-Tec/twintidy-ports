/*
 * scanner.h — duplicate-detection engine shared by the CLI and the GUI.
 *
 * The engine is read-only: it never deletes, moves, or modifies a file.
 * It is also UI-agnostic, so the same detection pipeline backs both the
 * command-line tool and the Windows front end.
 */

#ifndef TWINTIDY_SCANNER_H
#define TWINTIDY_SCANNER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *path;
    uint64_t size;
} td_file;

/* One confirmed duplicate group: files whose full contents matched. */
typedef struct {
    uint64_t size;
    td_file *files;
    size_t count;
} td_group;

typedef struct {
    td_group *groups;
    size_t count;
    uint64_t reclaimable;   /* bytes freed by keeping one copy per group */
    size_t files_scanned;
    size_t files_unreadable;
} td_result;

/*
 * Progress callback. `done`/`total` describe the current stage; `total` is
 * zero while walking, when the size of the work is not yet known.
 * Return 0 to request cancellation, non-zero to continue.
 */
typedef int (*td_progress_fn)(void *ctx, const char *stage, size_t done, size_t total);

#define TD_OK        0
#define TD_ERR_MEMORY 1
#define TD_ERR_ROOT   2
#define TD_CANCELLED  3

/*
 * Scan `root` recursively and report confirmed duplicate groups. `progress`
 * may be NULL. On TD_OK the caller owns `*out` and must release it with
 * td_result_free.
 */
int td_scan(const char *root, td_result *out, td_progress_fn progress, void *ctx);

void td_result_free(td_result *result);

#endif /* TWINTIDY_SCANNER_H */
