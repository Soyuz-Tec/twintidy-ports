/*
 * safety.c — protected-path policy and file-category classification.
 * See safety.h for the contract. The lists mirror TwinTidy's Go engine.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "safety.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Directory names that are never traversed, at any depth. */
static const char *protected_directories[] = {
    "$recycle.bin", "system volume information", "windows", "program files",
    "program files (x86)", "programdata", "recovery", "perflogs", "config.msi",
    "msocache", "appdata", "application data", "temporary internet files",
    "inetcache", "cache", ".cache", ".git", ".hg", ".svn", "node_modules",
    "__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache", ".venv",
    "venv", "env", "site-packages", "packages", "bin", "obj", "target",
    "build", "dist",
};

/* Extensions that are never duplicate candidates. */
static const char *protected_extensions[] = {
    ".386", ".appx", ".appxbundle", ".cab", ".com", ".cpl", ".cur", ".dll",
    ".drv", ".efi", ".exe", ".gadget", ".hta", ".icl", ".icns", ".ico",
    ".inf", ".ins", ".iso", ".job", ".lnk", ".msi", ".msix", ".msixbundle",
    ".msp", ".mst", ".ocx", ".pif", ".scr", ".sys", ".theme", ".themepack",
};

typedef struct {
    td_category category;
    const char *label;
    const char *const *extensions;
    size_t extension_count;
} category_definition;

static const char *pdf_ext[] = {".pdf"};
static const char *text_ext[] = {".txt", ".md", ".csv", ".tsv", ".json", ".xml",
                                 ".html", ".css", ".js", ".go", ".py", ".log",
                                 ".ini", ".yaml", ".yml", ".sql", ".ps1"};
static const char *word_ext[] = {".doc", ".docx", ".docm", ".rtf"};
static const char *excel_ext[] = {".xls", ".xlsx", ".xlsm", ".xlsb"};
static const char *ppt_ext[] = {".ppt", ".pptx", ".pptm"};
static const char *image_ext[] = {".bmp", ".dib", ".gif", ".jpg", ".jpeg", ".jpe",
                                  ".png", ".tif", ".tiff", ".ico", ".heic",
                                  ".heif", ".webp", ".raw", ".cr2", ".nef", ".arw"};
static const char *audio_ext[] = {".mp3", ".m4a", ".aac", ".wav", ".wma",
                                   ".flac", ".ogg", ".aiff"};
static const char *video_ext[] = {".mp4", ".m4v", ".mov", ".avi", ".mkv",
                                   ".wmv", ".webm", ".mpeg", ".mpg", ".3gp"};
static const char *archive_ext[] = {".zip", ".7z", ".rar", ".tar", ".gz",
                                     ".bz2", ".xz"};

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

static const category_definition categories[] = {
    {TD_CATEGORY_PDF, "PDF", pdf_ext, COUNT_OF(pdf_ext)},
    {TD_CATEGORY_TEXT, "Text", text_ext, COUNT_OF(text_ext)},
    {TD_CATEGORY_WORD, "Word", word_ext, COUNT_OF(word_ext)},
    {TD_CATEGORY_EXCEL, "Excel", excel_ext, COUNT_OF(excel_ext)},
    {TD_CATEGORY_POWERPOINT, "PowerPoint", ppt_ext, COUNT_OF(ppt_ext)},
    {TD_CATEGORY_IMAGES, "Images", image_ext, COUNT_OF(image_ext)},
    {TD_CATEGORY_AUDIO, "Audio", audio_ext, COUNT_OF(audio_ext)},
    {TD_CATEGORY_VIDEO, "Video", video_ext, COUNT_OF(video_ext)},
    {TD_CATEGORY_ARCHIVES, "Archives", archive_ext, COUNT_OF(archive_ext)},
    {TD_CATEGORY_OTHER, "Other", NULL, 0},
};

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

/* Return the extension including its dot, or "" when there is none. */
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

const char *td_category_label(td_category category) {
    for (size_t i = 0; i < COUNT_OF(categories); i++) {
        if (categories[i].category == category) {
            return categories[i].label;
        }
    }
    return "Other";
}

td_category td_category_for_path(const char *path) {
    const char *extension = extension_of(path);
    if (*extension == '\0') {
        return TD_CATEGORY_OTHER;
    }
    for (size_t i = 0; i < COUNT_OF(categories); i++) {
        for (size_t e = 0; e < categories[i].extension_count; e++) {
            if (equals_fold(extension, categories[i].extensions[e])) {
                return categories[i].category;
            }
        }
    }
    return TD_CATEGORY_OTHER;
}

/* Compare one path segment, delimited by separators or the string end. */
static int segment_is_protected(const char *segment, size_t length) {
    for (size_t i = 0; i < COUNT_OF(protected_directories); i++) {
        const char *candidate = protected_directories[i];
        if (strlen(candidate) != length) {
            continue;
        }
        size_t index = 0;
        while (index < length &&
               tolower((unsigned char)segment[index]) == candidate[index]) {
            index++;
        }
        if (index == length) {
            return 1;
        }
    }
    return 0;
}

int td_should_skip_directory(const char *path) {
    if (path == NULL || *path == '\0') {
        return 0;
    }
    const char *cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/' || *cursor == '\\') {
            cursor++;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != '/' && *cursor != '\\') {
            cursor++;
        }
        size_t length = (size_t)(cursor - start);
        /* Skip a bare drive prefix such as "C:" so it is never a segment. */
        if (length > 0 && !(length == 2 && start[1] == ':')) {
            if (segment_is_protected(start, length)) {
                return 1;
            }
        }
    }
    return 0;
}

int td_is_user_created_file(const char *path) {
    if (path == NULL || *path == '\0') {
        return 0;
    }
    const char *extension = extension_of(path);
    for (size_t i = 0; i < COUNT_OF(protected_extensions); i++) {
        if (equals_fold(extension, protected_extensions[i])) {
            return 0;
        }
    }
    /* A file inherits its directory's protection. */
    const char *last_separator = NULL;
    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
            last_separator = cursor;
        }
    }
    if (last_separator != NULL) {
        size_t directory_length = (size_t)(last_separator - path);
        char *directory = malloc(directory_length + 1);
        if (directory == NULL) {
            return 0;
        }
        memcpy(directory, path, directory_length);
        directory[directory_length] = '\0';
        int skip = td_should_skip_directory(directory);
        free(directory);
        if (skip) {
            return 0;
        }
    }
    return 1;
}
