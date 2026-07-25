/*
 * unit.c — unit tests for the pure functions of the C port.
 *
 * The smoke script exercises the CLI end to end; these tests cover the
 * decision logic underneath it, where an end-to-end test cannot show which
 * rule fired or isolate a boundary case.
 *
 * Build and run:  make unit-test
 */

#define _DEFAULT_SOURCE

#include "../src/report.h"
#include "../src/safety.h"
#include "../src/scanner.h"
#include "../src/settings.h"
#include "../src/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

static void check(int condition, const char *what) {
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_str(const char *actual, const char *expected, const char *what) {
    checks++;
    if (actual == NULL || strcmp(actual, expected) != 0) {
        failures++;
        printf("  FAIL: %s\n    expected: %s\n    actual:   %s\n", what, expected,
               actual ? actual : "(null)");
    }
}

/* ---------- sha256 ---------- */

static void sha256_of(const char *input, char *out) {
    td_sha256 context;
    td_sha256_init(&context);
    td_sha256_update(&context, (const unsigned char *)input, strlen(input));
    unsigned char digest[TD_SHA256_DIGEST_SIZE];
    td_sha256_final(&context, digest);
    td_sha256_hex(digest, out);
}

static void test_sha256(void) {
    puts("sha256");
    char hex[TD_SHA256_HEX_SIZE];

    /* FIPS 180-4 vectors. */
    sha256_of("", hex);
    check_str(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "empty string");
    sha256_of("abc", hex);
    check_str(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    sha256_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
    check_str(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "two-block vector");

    /* Streaming in odd-sized chunks must equal a single-shot digest. */
    char single[TD_SHA256_HEX_SIZE];
    char streamed[TD_SHA256_HEX_SIZE];
    char payload[1000];
    for (size_t i = 0; i < sizeof payload - 1; i++) {
        payload[i] = (char)('a' + (i % 26));
    }
    payload[sizeof payload - 1] = '\0';
    sha256_of(payload, single);

    td_sha256 context;
    td_sha256_init(&context);
    for (size_t offset = 0; offset < strlen(payload); offset += 37) {
        size_t chunk = strlen(payload) - offset;
        if (chunk > 37) {
            chunk = 37;
        }
        td_sha256_update(&context, (const unsigned char *)payload + offset, chunk);
    }
    unsigned char digest[TD_SHA256_DIGEST_SIZE];
    td_sha256_final(&context, digest);
    td_sha256_hex(digest, streamed);
    check_str(streamed, single, "streamed digest equals single-shot");
}

/* ---------- safety ---------- */

static void test_safety(void) {
    puts("safety");

    check(td_should_skip_directory("C:\\code\\project\\node_modules"),
          "node_modules is protected at depth");
    check(td_should_skip_directory("C:\\code\\.git\\refs"), ".git is protected");
    check(td_should_skip_directory("/home/a/target/debug"), "target is protected");
    check(!td_should_skip_directory("C:\\code\\project\\src"), "ordinary folder is allowed");
    /* A drive prefix must never be mistaken for a protected segment. */
    check(!td_should_skip_directory("C:\\Users\\example"), "drive prefix is not a segment");

    check(!td_is_user_created_file("C:\\Users\\a\\setup.exe"), ".exe is protected");
    check(!td_is_user_created_file("C:\\Users\\a\\lib.DLL"), "extension match is case-insensitive");
    check(!td_is_user_created_file("C:\\code\\node_modules\\p\\index.js"),
          "a file inherits its folder's protection");
    check(td_is_user_created_file("C:\\Users\\a\\notes.txt"), "ordinary file is allowed");

    check(td_category_for_path("a.PDF") == TD_CATEGORY_PDF, "PDF classified case-insensitively");
    check(td_category_for_path("a.jpeg") == TD_CATEGORY_IMAGES, "jpeg is an image");
    check(td_category_for_path("a.unknown") == TD_CATEGORY_OTHER, "unknown extension is Other");
    check(td_category_for_path("noextension") == TD_CATEGORY_OTHER, "no extension is Other");
    check_str(td_category_label(TD_CATEGORY_POWERPOINT), "PowerPoint", "category label");
}

/* ---------- report ---------- */

static void test_report_formula_guard(void) {
    puts("report");

    /* A path beginning with a formula character must be neutralized so a
     * spreadsheet cannot evaluate it. */
    td_result result;
    memset(&result, 0, sizeof result);
    td_file files[2];
    memset(files, 0, sizeof files);
    files[0].path = (char *)"=cmd|calc";
    files[0].size = 10;
    files[1].path = (char *)"plain.txt";
    files[1].size = 10;
    td_group group;
    memset(&group, 0, sizeof group);
    group.size = 10;
    memcpy(group.hash, "abc123", 7);
    group.files = files;
    group.count = 2;
    result.groups = &group;
    result.count = 1;
    result.reclaimable = 10;

    char name[L_tmpnam];
    if (tmpnam(name) == NULL) {
        check(0, "could not name a temporary file");
        return;
    }
    check(td_report_write_file(name, TD_REPORT_CSV, "scan", &result), "CSV report written");

    FILE *file = fopen(name, "rb");
    check(file != NULL, "CSV report readable");
    if (file != NULL) {
        char body[4096];
        size_t read = fread(body, 1, sizeof body - 1, file);
        body[read] = '\0';
        fclose(file);
        check(strstr(body, "\"'=cmd|calc\"") != NULL, "formula-leading path is guarded");
        check(strstr(body, "generatedAt,scanFolder") == body, "header comes first");
    }
    remove(name);
}

/* ---------- settings ---------- */

static void test_settings(void) {
    puts("settings");

    char name[L_tmpnam];
    if (tmpnam(name) == NULL) {
        check(0, "could not name a temporary file");
        return;
    }

    /* Missing file must yield defaults rather than an error. */
    td_settings missing = td_settings_load("this-file-does-not-exist.ini");
    check(missing.width == 0 && missing.height == 0, "missing file yields defaults");

    td_settings value = td_settings_defaults();
    value.x = 100;
    value.y = 200;
    value.width = 1180;
    value.height = 720;
    value.maximized = 1;
    snprintf(value.last_folder, sizeof value.last_folder, "C:\\Users\\example");
    check(td_settings_save(name, &value), "settings saved");

    td_settings restored = td_settings_load(name);
    check(restored.x == 100 && restored.y == 200, "position round-trips");
    check(restored.width == 1180 && restored.height == 720, "extent round-trips");
    check(restored.maximized == 1, "maximized round-trips");
    check_str(restored.last_folder, "C:\\Users\\example", "last folder round-trips");
    remove(name);

    /* A placement with no extent is meaningless and must be discarded. */
    FILE *file = fopen(name, "wb");
    if (file != NULL) {
        fputs("x=50\ny=60\nwidth=0\nheight=0\ngarbage line\n", file);
        fclose(file);
        td_settings partial = td_settings_load(name);
        check(partial.x == 0 && partial.width == 0, "placement without extent is discarded");
        remove(name);
    }
}

/* ---------- options ---------- */

static void test_default_options(void) {
    puts("options");
    td_options options = td_default_options();
    check(options.max_files > 0 && options.max_directories > 0,
          "defaults set cardinality limits");
    check(options.min_file_size == 0, "no size floor by default");
    int any_category = 0;
    for (size_t i = 0; i < TD_CATEGORY_COUNT; i++) {
        any_category |= options.categories[i];
    }
    check(!any_category, "no explicit category selection means all categories");
}

int main(void) {
    test_sha256();
    test_safety();
    test_report_formula_guard();
    test_settings();
    test_default_options();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);
    if (failures > 0) {
        puts("UNIT TESTS FAILED");
        return 1;
    }
    puts("UNIT TESTS OK");
    return 0;
}
