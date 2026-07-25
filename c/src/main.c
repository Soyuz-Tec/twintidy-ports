/*
 * main.c — command-line front end for the TwinTidy C port.
 *
 * Read-only: reports duplicate groups and never modifies the scanned tree.
 *
 * Usage: twintidy <folder>
 */

#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <folder>\n", argv[0]);
        return 2;
    }

    td_result result;
    int status = td_scan(argv[1], &result, NULL, NULL);
    if (status == TD_ERR_MEMORY) {
        fprintf(stderr, "out of memory while scanning\n");
        return 2;
    }
    if (status != TD_OK) {
        fprintf(stderr, "scan failed\n");
        return 2;
    }

    if (result.files_scanned == 0) {
        puts("No files found.");
        td_result_free(&result);
        return 0;
    }

    for (size_t g = 0; g < result.count; g++) {
        printf("\nDuplicate group %zu  (%llu bytes each, %zu files)\n",
               g + 1,
               (unsigned long long)result.groups[g].size,
               result.groups[g].count);
        for (size_t f = 0; f < result.groups[g].count; f++) {
            printf("  %s\n", result.groups[g].files[f].path);
        }
    }

    if (result.count == 0) {
        puts("No duplicates found.");
    } else {
        printf("\n%zu duplicate group(s); keeping one copy of each would "
               "reclaim %llu bytes.\n",
               result.count, (unsigned long long)result.reclaimable);
    }
    if (result.files_unreadable > 0) {
        fprintf(stderr, "warn: %zu item(s) could not be read and were skipped\n",
                result.files_unreadable);
    }

    td_result_free(&result);
    return 0;
}
