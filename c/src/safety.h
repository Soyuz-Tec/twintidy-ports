/*
 * safety.h — the file-selection policy ported from TwinTidy's Go engine.
 *
 * Two independent rules keep a scan on user-created content:
 *   - protected directories: system, application, and build-output folders
 *     are never traversed;
 *   - protected extensions: executables, drivers, installers, and shortcuts
 *     are never treated as duplicate candidates.
 *
 * Without this policy a scan reports build artefacts and repository
 * internals that a user must not act on.
 */

#ifndef TWINTIDY_SAFETY_H
#define TWINTIDY_SAFETY_H

/* File categories, matching the Go engine's classification. */
typedef enum {
    TD_CATEGORY_PDF = 0,
    TD_CATEGORY_TEXT,
    TD_CATEGORY_WORD,
    TD_CATEGORY_EXCEL,
    TD_CATEGORY_POWERPOINT,
    TD_CATEGORY_IMAGES,
    TD_CATEGORY_AUDIO,
    TD_CATEGORY_VIDEO,
    TD_CATEGORY_ARCHIVES,
    TD_CATEGORY_OTHER,
    TD_CATEGORY_COUNT
} td_category;

/* Stable display label for a category, e.g. "PowerPoint". */
const char *td_category_label(td_category category);

/* Classify a path by extension; unknown extensions map to TD_CATEGORY_OTHER. */
td_category td_category_for_path(const char *path);

/*
 * Report whether a directory must not be traversed. Any path segment that
 * matches a protected name disqualifies the whole subtree, so a nested
 * node_modules or .git is skipped wherever it appears.
 */
int td_should_skip_directory(const char *path);

/*
 * Report whether a file may be treated as a duplicate candidate. Files with
 * protected extensions and files inside protected directories are excluded.
 */
int td_is_user_created_file(const char *path);

#endif /* TWINTIDY_SAFETY_H */
