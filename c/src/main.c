/*
 * main.c — command-line front end for the TwinTidy C port.
 *
 * Read-only: reports duplicate groups and never modifies the scanned tree.
 */

#define _DEFAULT_SOURCE

#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *program) {
    fprintf(stderr, "usage: %s [options] <folder>\n\n", program);
    fprintf(stderr, "Find exact duplicate files. The scan is read-only.\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --min-size BYTES     ignore files smaller than BYTES\n");
    fprintf(stderr, "  --exclude PATH       skip this path subtree (repeatable)\n");
    fprintf(stderr, "  --exclude-ext EXT    skip this extension, e.g. .tmp (repeatable)\n");
    fprintf(stderr, "  --category NAME      restrict to a category (repeatable);\n");
    fprintf(stderr, "                       pdf text word excel powerpoint images\n");
    fprintf(stderr, "                       audio video archives other\n");
    fprintf(stderr, "  --surface            report the surface inventory and exit\n");
    fprintf(stderr, "  --help               show this help\n\n");
    fprintf(stderr, "Exit codes: 0 no duplicates, 1 duplicates found, 2 error.\n");
}

static int category_from_name(const char *name, td_category *out) {
    static const struct {
        const char *name;
        td_category category;
    } lookup[] = {
        {"pdf", TD_CATEGORY_PDF},         {"text", TD_CATEGORY_TEXT},
        {"word", TD_CATEGORY_WORD},       {"excel", TD_CATEGORY_EXCEL},
        {"powerpoint", TD_CATEGORY_POWERPOINT}, {"images", TD_CATEGORY_IMAGES},
        {"audio", TD_CATEGORY_AUDIO},     {"video", TD_CATEGORY_VIDEO},
        {"archives", TD_CATEGORY_ARCHIVES}, {"other", TD_CATEGORY_OTHER},
    };
    for (size_t i = 0; i < sizeof lookup / sizeof *lookup; i++) {
        if (strcmp(lookup[i].name, name) == 0) {
            *out = lookup[i].category;
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    td_options options = td_default_options();
    const char *excluded_paths[64];
    const char *excluded_extensions[64];
    size_t excluded_path_count = 0;
    size_t excluded_extension_count = 0;
    const char *root = NULL;
    int surface_only = 0;

    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argument, "--surface") == 0) {
            surface_only = 1;
        } else if (strcmp(argument, "--min-size") == 0 && i + 1 < argc) {
            options.min_file_size = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argument, "--exclude") == 0 && i + 1 < argc) {
            if (excluded_path_count < 64) {
                excluded_paths[excluded_path_count++] = argv[++i];
            } else {
                i++;
            }
        } else if (strcmp(argument, "--exclude-ext") == 0 && i + 1 < argc) {
            if (excluded_extension_count < 64) {
                excluded_extensions[excluded_extension_count++] = argv[++i];
            } else {
                i++;
            }
        } else if (strcmp(argument, "--category") == 0 && i + 1 < argc) {
            td_category category;
            if (!category_from_name(argv[++i], &category)) {
                fprintf(stderr, "unknown category: %s\n", argv[i]);
                return 2;
            }
            options.categories[category] = 1;
        } else if (argument[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argument);
            print_usage(argv[0]);
            return 2;
        } else if (root == NULL) {
            root = argument;
        } else {
            fprintf(stderr, "only one folder may be scanned\n");
            return 2;
        }
    }

    if (root == NULL) {
        print_usage(argv[0]);
        return 2;
    }
    options.excluded_paths = excluded_paths;
    options.excluded_path_count = excluded_path_count;
    options.excluded_extensions = excluded_extensions;
    options.excluded_extension_count = excluded_extension_count;

    td_surface surface;
    int status = td_surface_scan(root, &options, &surface, NULL, NULL);
    if (status == TD_ERR_ROOT) {
        fprintf(stderr, "cannot scan %s: the folder is a protected system or build location\n", root);
        return 2;
    }
    if (status != TD_OK) {
        fprintf(stderr, "surface scan failed\n");
        return 2;
    }

    if (surface_only) {
        printf("Surface inventory for %s\n", root);
        printf("  %zu user file(s), %llu byte(s) in %zu folder(s)\n",
               surface.count, (unsigned long long)surface.total_bytes,
               surface.directories_scanned);
        printf("  %zu protected item(s) skipped, %zu unreadable\n\n",
               surface.skipped_system_items, surface.errors_ignored);
        for (size_t c = 0; c < TD_CATEGORY_COUNT; c++) {
            if (surface.category_stats[c].files == 0) {
                continue;
            }
            printf("  %-12s %6zu file(s)  %llu byte(s)\n",
                   td_category_label((td_category)c),
                   surface.category_stats[c].files,
                   (unsigned long long)surface.category_stats[c].bytes);
        }
        td_surface_free(&surface);
        return 0;
    }

    td_result result;
    status = td_find_duplicates(&surface, &options, &result, NULL, NULL);
    td_surface_free(&surface);
    if (status != TD_OK) {
        fprintf(stderr, "duplicate scan failed\n");
        return 2;
    }

    for (size_t g = 0; g < result.count; g++) {
        printf("\nDuplicate group %zu  (%llu bytes each, %zu files)\n",
               g + 1, (unsigned long long)result.groups[g].size,
               result.groups[g].count);
        printf("  sha256 %s\n", result.groups[g].hash);
        for (size_t f = 0; f < result.groups[g].count; f++) {
            printf("  %s\n", result.groups[g].files[f].path);
        }
    }

    int exit_code;
    if (result.count == 0) {
        printf("No duplicates found among %zu considered file(s).\n",
               result.files_considered);
        exit_code = 0;
    } else {
        printf("\n%zu duplicate group(s); keeping one copy of each would "
               "reclaim %llu bytes.\n",
               result.count, (unsigned long long)result.reclaimable);
        exit_code = 1;
    }
    if (result.files_unreadable > 0) {
        fprintf(stderr, "warn: %zu file(s) could not be read and were skipped\n",
                result.files_unreadable);
    }

    td_result_free(&result);
    return exit_code;
}
