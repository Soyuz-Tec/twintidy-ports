/*
 * twintidy.c — minimal C port of TwinTidy's duplicate-detection core.
 *
 * Strategy (same as the Go engine):
 *   1. Walk the tree, collect regular files.
 *   2. Group by exact size (different size => not duplicates).
 *   3. Within a size group, hash each file (FNV-1a 64, streamed).
 *   4. Within a hash group, confirm with a full byte-for-byte compare,
 *      so a hash collision can never produce a false duplicate.
 *
 * Read-only: this program never deletes, moves, or modifies anything.
 *
 * Build (MinGW-w64 or any C11 compiler with dirent.h):
 *   gcc -std=c11 -O2 -o twintidy twintidy.c
 * Usage:
 *   twintidy <folder>
 */

/* Expose POSIX declarations (strdup, lstat) under strict -std=c11 on
 * glibc; without this they are implicitly declared and the returned
 * pointer from strdup is truncated to int, crashing on 64-bit Linux. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

typedef struct FileEntry {
    char *path;
    uint64_t size;
    uint64_t hash;
    int hashed;      /* hash computed? */
    int claimed;     /* already assigned to a duplicate group? */
} FileEntry;

static FileEntry *files = NULL;
static size_t nfiles = 0, capfiles = 0;

static void add_file(const char *path, uint64_t size) {
    if (nfiles == capfiles) {
        capfiles = capfiles ? capfiles * 2 : 256;
        files = realloc(files, capfiles * sizeof *files);
        if (!files) { perror("realloc"); exit(1); }
    }
    files[nfiles].path = strdup(path);
    if (!files[nfiles].path) { perror("strdup"); exit(1); }
    files[nfiles].size = size;
    files[nfiles].hashed = 0;
    files[nfiles].claimed = 0;
    nfiles++;
}

/* Recursively walk dir, collecting regular files. Symlinks are skipped
 * via lstat where available so cycles cannot occur. */
static void walk(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "warn: cannot open %s\n", dir); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        size_t len = strlen(dir) + 1 + strlen(e->d_name) + 1;
        char *p = malloc(len);
        if (!p) { perror("malloc"); exit(1); }
        snprintf(p, len, "%s%c%s", dir, PATH_SEP, e->d_name);
        struct stat st;
#ifdef _WIN32
        if (stat(p, &st) != 0) { free(p); continue; }
#else
        if (lstat(p, &st) != 0) { free(p); continue; }
        if (S_ISLNK(st.st_mode)) { free(p); continue; }
#endif
        if (S_ISDIR(st.st_mode)) {
            walk(p);
        } else if (S_ISREG(st.st_mode)) {
            add_file(p, (uint64_t)st.st_size);
        }
        free(p);
    }
    closedir(d);
}

/* Streamed FNV-1a 64-bit hash of a whole file. Returns 0 on I/O error
 * with *ok cleared; callers must skip unhashable files, never guess. */
static uint64_t fnv1a_file(const char *path, int *ok) {
    *ok = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t h = 1469598103934665603ULL;
    unsigned char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
    }
    int err = ferror(f);
    fclose(f);
    if (err) return 0;
    *ok = 1;
    return h;
}

/* Byte-for-byte comparison; the final arbiter, immune to collisions. */
static int same_bytes(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = 0;
    if (fa && fb) {
        unsigned char ba[1 << 16], bb[1 << 16];
        size_t na, nb;
        same = 1;
        do {
            na = fread(ba, 1, sizeof ba, fa);
            nb = fread(bb, 1, sizeof bb, fb);
            if (na != nb || memcmp(ba, bb, na) != 0) { same = 0; break; }
        } while (na > 0);
        if (ferror(fa) || ferror(fb)) same = 0;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

static int cmp_size(const void *x, const void *y) {
    const FileEntry *a = x, *b = y;
    if (a->size != b->size) return a->size < b->size ? -1 : 1;
    return strcmp(a->path, b->path);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <folder>\n", argv[0]);
        return 2;
    }
    walk(argv[1]);
    if (nfiles == 0) { puts("No files found."); return 0; }

    qsort(files, nfiles, sizeof *files, cmp_size);

    int groups = 0;
    uint64_t reclaimable = 0;

    for (size_t i = 0; i < nfiles; ) {
        /* [i, j) is one size group */
        size_t j = i + 1;
        while (j < nfiles && files[j].size == files[i].size) j++;
        if (j - i >= 2 && files[i].size > 0) {
            /* hash candidates lazily inside the size group */
            for (size_t k = i; k < j; k++) {
                int ok;
                files[k].hash = fnv1a_file(files[k].path, &ok);
                files[k].hashed = ok;
                if (!ok)
                    fprintf(stderr, "warn: cannot read %s (skipped)\n",
                            files[k].path);
            }
            for (size_t k = i; k < j; k++) {
                if (!files[k].hashed || files[k].claimed) continue;
                /* collect confirmed duplicates of files[k] */
                size_t members[256];
                size_t m = 0;
                members[m++] = k;
                for (size_t l = k + 1; l < j && m < 256; l++) {
                    if (!files[l].hashed || files[l].claimed) continue;
                    if (files[l].hash != files[k].hash) continue;
                    if (same_bytes(files[k].path, files[l].path)) {
                        files[l].claimed = 1;
                        members[m++] = l;
                    }
                }
                if (m >= 2) {
                    groups++;
                    reclaimable += files[k].size * (m - 1);
                    printf("\nDuplicate group %d  (%llu bytes each, %zu files)\n",
                           groups, (unsigned long long)files[k].size, m);
                    for (size_t t = 0; t < m; t++)
                        printf("  %s\n", files[members[t]].path);
                }
            }
        }
        i = j;
    }

    if (groups == 0)
        puts("No duplicates found.");
    else
        printf("\n%d duplicate group(s); keeping one copy of each would "
               "reclaim %llu bytes.\n",
               groups, (unsigned long long)reclaimable);

    for (size_t k = 0; k < nfiles; k++) free(files[k].path);
    free(files);
    return 0;
}
