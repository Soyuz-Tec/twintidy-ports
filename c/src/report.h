/*
 * report.h — duplicate-report export, matching the Go engine's formats.
 *
 * CSV cells that spreadsheet software would evaluate as formulas are
 * neutralized, so an exported report cannot execute anything when opened.
 */

#ifndef TWINTIDY_REPORT_H
#define TWINTIDY_REPORT_H

#include <stdio.h>

#include "scanner.h"

typedef enum {
    TD_REPORT_CSV = 0,
    TD_REPORT_JSON
} td_report_format;

/* Conventional file extension for a format, including the dot. */
const char *td_report_extension(td_report_format format);

/*
 * Write the report for `result` to `out`. `folder` labels the scan root and
 * may be NULL. Returns non-zero on success.
 */
int td_report_write(FILE *out, td_report_format format, const char *folder,
                    const td_result *result);

/*
 * Write atomically: the report is built in a temporary file beside `path`
 * and renamed into place only after it is complete, so a failure never
 * leaves a truncated report at the destination. Returns non-zero on success.
 */
int td_report_write_file(const char *path, td_report_format format,
                         const char *folder, const td_result *result);

#endif /* TWINTIDY_REPORT_H */
