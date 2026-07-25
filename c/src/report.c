/* report.c — CSV and JSON duplicate-report export. See report.h. */

#define _DEFAULT_SOURCE

#include "report.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REPORT_SCHEMA "twintidy.duplicate-report/v1"

const char *td_report_extension(td_report_format format) {
    return format == TD_REPORT_JSON ? ".json" : ".csv";
}

/*
 * Spreadsheet software evaluates a cell beginning with one of these as a
 * formula or command. Prefixing with an apostrophe keeps the text visible
 * while preventing evaluation.
 */
static int needs_formula_guard(const char *value) {
    if (value == NULL || *value == '\0') {
        return 0;
    }
    switch (*value) {
    case '=':
    case '+':
    case '-':
    case '@':
    case '\t':
    case '\r':
        return 1;
    default:
        return 0;
    }
}

static void write_csv_cell(FILE *out, const char *value) {
    if (value == NULL) {
        value = "";
    }
    fputc('"', out);
    if (needs_formula_guard(value)) {
        fputc('\'', out);
    }
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor == '"') {
            fputc('"', out); /* RFC 4180 doubling */
        }
        fputc(*cursor, out);
    }
    fputc('"', out);
}

static void write_json_string(FILE *out, const char *value) {
    if (value == NULL) {
        value = "";
    }
    fputc('"', out);
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        switch (*cursor) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (*cursor < 0x20) {
                fprintf(out, "\\u%04x", *cursor);
            } else {
                fputc((int)*cursor, out);
            }
        }
    }
    fputc('"', out);
}

static void format_timestamp(int64_t seconds, char *out, size_t size) {
    if (seconds <= 0) {
        out[0] = '\0';
        return;
    }
    time_t value = (time_t)seconds;
    struct tm parts;
#ifdef _WIN32
    if (gmtime_s(&parts, &value) != 0) {
        out[0] = '\0';
        return;
    }
#else
    if (gmtime_r(&value, &parts) == NULL) {
        out[0] = '\0';
        return;
    }
#endif
    if (strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &parts) == 0) {
        out[0] = '\0';
    }
}

static void generated_at(char *out, size_t size) {
    format_timestamp((int64_t)time(NULL), out, size);
}

static uint64_t group_reclaimable(const td_group *group) {
    if (group->count < 2) {
        return 0;
    }
    return group->size * (uint64_t)(group->count - 1);
}

static int write_csv(FILE *out, const char *folder, const td_result *result) {
    char stamp[64];
    generated_at(stamp, sizeof stamp);

    fputs("generatedAt,scanFolder,group,sha256,groupSize,groupReclaimableBytes,"
          "reportReclaimableBytes,path,fileSize,modifiedAt,category\r\n",
          out);

    int report_total_written = 0;
    for (size_t g = 0; g < result->count; g++) {
        const td_group *group = &result->groups[g];
        for (size_t f = 0; f < group->count; f++) {
            const td_file *file = &group->files[f];
            char modified[64];
            format_timestamp(file->modified_at, modified, sizeof modified);

            write_csv_cell(out, stamp);
            fputc(',', out);
            write_csv_cell(out, folder);
            fprintf(out, ",%zu,", g + 1);
            write_csv_cell(out, group->hash);
            fprintf(out, ",%llu,", (unsigned long long)group->size);
            /* Per-group and per-report totals are emitted once each, so a
             * reader can sum the file rows without double counting. */
            if (f == 0) {
                fprintf(out, "%llu", (unsigned long long)group_reclaimable(group));
            }
            fputc(',', out);
            if (!report_total_written) {
                fprintf(out, "%llu", (unsigned long long)result->reclaimable);
                report_total_written = 1;
            }
            fputc(',', out);
            write_csv_cell(out, file->path);
            fprintf(out, ",%llu,", (unsigned long long)file->size);
            write_csv_cell(out, modified);
            fputc(',', out);
            write_csv_cell(out, td_category_label(file->category));
            fputs("\r\n", out);
        }
    }
    return ferror(out) == 0;
}

static int write_json(FILE *out, const char *folder, const td_result *result) {
    char stamp[64];
    generated_at(stamp, sizeof stamp);

    size_t file_count = 0;
    for (size_t g = 0; g < result->count; g++) {
        file_count += result->groups[g].count;
    }

    fputs("{\n  \"schema\": ", out);
    write_json_string(out, REPORT_SCHEMA);
    fputs(",\n  \"generatedAt\": ", out);
    write_json_string(out, stamp);
    if (folder != NULL && *folder != '\0') {
        fputs(",\n  \"folder\": ", out);
        write_json_string(out, folder);
    }
    fprintf(out, ",\n  \"groupCount\": %zu", result->count);
    fprintf(out, ",\n  \"fileCount\": %zu", file_count);
    fprintf(out, ",\n  \"reclaimableBytes\": %llu",
            (unsigned long long)result->reclaimable);
    fputs(",\n  \"groups\": [", out);

    for (size_t g = 0; g < result->count; g++) {
        const td_group *group = &result->groups[g];
        fputs(g > 0 ? ",\n    {" : "\n    {", out);
        fprintf(out, "\n      \"size\": %llu", (unsigned long long)group->size);
        fputs(",\n      \"sha256\": ", out);
        write_json_string(out, group->hash);
        fputs(",\n      \"files\": [", out);
        for (size_t f = 0; f < group->count; f++) {
            const td_file *file = &group->files[f];
            char modified[64];
            format_timestamp(file->modified_at, modified, sizeof modified);

            fputs(f > 0 ? ",\n        {" : "\n        {", out);
            fputs("\"path\": ", out);
            write_json_string(out, file->path);
            fprintf(out, ", \"size\": %llu", (unsigned long long)file->size);
            fputs(", \"modifiedAt\": ", out);
            write_json_string(out, modified);
            fputs(", \"category\": ", out);
            write_json_string(out, td_category_label(file->category));
            fputc('}', out);
        }
        fputs(group->count > 0 ? "\n      ]" : "]", out);
        fputs("\n    }", out);
    }
    fputs(result->count > 0 ? "\n  ]\n}\n" : "]\n}\n", out);
    return ferror(out) == 0;
}

int td_report_write(FILE *out, td_report_format format, const char *folder,
                    const td_result *result) {
    if (out == NULL || result == NULL) {
        return 0;
    }
    if (format == TD_REPORT_JSON) {
        return write_json(out, folder, result);
    }
    return write_csv(out, folder, result);
}

int td_report_write_file(const char *path, td_report_format format,
                         const char *folder, const td_result *result) {
    if (path == NULL || result == NULL) {
        return 0;
    }
    size_t length = strlen(path) + 5;
    char *staging = malloc(length);
    if (staging == NULL) {
        return 0;
    }
    snprintf(staging, length, "%s.tmp", path);

    FILE *out = fopen(staging, "wb");
    if (out == NULL) {
        free(staging);
        return 0;
    }
    int ok = td_report_write(out, format, folder, result);
    if (fclose(out) != 0) {
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
