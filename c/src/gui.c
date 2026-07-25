/*
 * gui.c — native Win32 front end for the TwinTidy C port.
 *
 * Read-only by construction: this window selects a folder, runs the
 * two-phase scan, and reviews the results. It exposes no action that
 * deletes, moves, or modifies a file; checkbox selection exists so a user
 * can plan cleanup and export the plan, not perform it.
 *
 * Scans run on a worker thread and report progress by posting messages to
 * the UI thread, so the window stays responsive and cancellable.
 *
 * Windows only. Build: make gui
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include "report.h"
#include "scanner.h"

/* windows.h must precede the Win32 sub-headers: they depend on its types. */
#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_SELECT_FOLDER 1001
#define ID_SURFACE_SCAN  1002
#define ID_FIND          1003
#define ID_CANCEL        1004
#define ID_LIST          1005
#define ID_KEEP_NEWEST   1006
#define ID_KEEP_OLDEST   1007
#define ID_CLEAR_SELECT  1008
#define ID_EXPORT        1009
#define ID_EXPLORER      1010
#define ID_CATEGORY_BASE 1100

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_SURFACE  (WM_APP + 2)
#define WM_APP_RESULT   (WM_APP + 3)
#define ID_ELAPSED_TIMER 1

/* One display row: a single file inside a duplicate group. */
typedef struct {
    int group;
    uint64_t size;
    int64_t modified_at;
    int checked;
    wchar_t *name;
    wchar_t *folder;
} row;

typedef struct {
    wchar_t stage[64];
    size_t done;
    size_t total;
} progress_message;

typedef struct {
    HWND main, select_button, surface_button, find_button, cancel_button;
    HWND keep_newest_button, keep_oldest_button, clear_select_button;
    HWND export_button, explorer_button;
    HWND list, progress, status, path_label, stage_label, elapsed_label, focus_label;
    HWND category_checks[TD_CATEGORY_COUNT];
    HFONT font;

    wchar_t folder[MAX_PATH];
    int has_folder;

    td_surface surface;
    int has_surface;
    td_result result;
    int has_result;

    row *rows;
    size_t row_count;

    HANDLE thread;
    volatile LONG cancel_requested;
    volatile LONG scanning;
    ULONGLONG started_at;
} app_state;

static app_state app;

/* ---------- string helpers ---------- */

/*
 * The engine stays portable and yields multi-byte paths, so display strings
 * are converted with the active code page. Paths containing characters
 * outside that code page are shown with substitutions; this affects display
 * only, never which files the engine compares.
 */
static wchar_t *widen(const char *text) {
    int needed = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
    if (needed <= 0) {
        return NULL;
    }
    wchar_t *wide = calloc((size_t)needed, sizeof *wide);
    if (wide == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide, needed);
    return wide;
}

static void narrow_into(const wchar_t *wide, char *out, size_t out_size) {
    WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int)out_size, NULL, NULL);
}

static void format_bytes(uint64_t bytes, wchar_t *out, size_t count) {
    const wchar_t *units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof units / sizeof *units) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) {
        _snwprintf(out, count, L"%llu B", (unsigned long long)bytes);
    } else {
        _snwprintf(out, count, L"%.1f %s", value, units[unit]);
    }
}

static void format_local_time(int64_t seconds, wchar_t *out, size_t count) {
    if (seconds <= 0) {
        out[0] = L'\0';
        return;
    }
    /* Unix seconds -> FILETIME 100ns ticks since 1601. */
    ULONGLONG ticks = (ULONGLONG)(seconds + 11644473600LL) * 10000000ULL;
    FILETIME utc;
    utc.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFULL);
    utc.dwHighDateTime = (DWORD)(ticks >> 32);
    FILETIME local;
    SYSTEMTIME parts;
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &parts)) {
        out[0] = L'\0';
        return;
    }
    _snwprintf(out, count, L"%04d-%02d-%02d %02d:%02d", parts.wYear, parts.wMonth,
               parts.wDay, parts.wHour, parts.wMinute);
}

static void set_status(const wchar_t *text) { SetWindowTextW(app.status, text); }

/* ---------- row model ---------- */

static void free_rows(void) {
    for (size_t i = 0; i < app.row_count; i++) {
        free(app.rows[i].name);
        free(app.rows[i].folder);
    }
    free(app.rows);
    app.rows = NULL;
    app.row_count = 0;
}

static void clear_result(void) {
    free_rows();
    if (app.has_result) {
        td_result_free(&app.result);
        app.has_result = 0;
    }
    if (app.list != NULL) {
        ListView_SetItemCountEx(app.list, 0, 0);
        InvalidateRect(app.list, NULL, TRUE);
    }
}

static void clear_surface(void) {
    clear_result();
    if (app.has_surface) {
        td_surface_free(&app.surface);
        app.has_surface = 0;
    }
}

/* Split each path into file name and containing folder so both columns
 * stay readable. */
static void build_rows(void) {
    free_rows();
    size_t total = 0;
    for (size_t g = 0; g < app.result.count; g++) {
        total += app.result.groups[g].count;
    }
    if (total == 0) {
        return;
    }
    app.rows = calloc(total, sizeof *app.rows);
    if (app.rows == NULL) {
        return;
    }

    size_t index = 0;
    for (size_t g = 0; g < app.result.count; g++) {
        for (size_t f = 0; f < app.result.groups[g].count; f++) {
            const td_file *file = &app.result.groups[g].files[f];
            wchar_t *full = widen(file->path);
            if (full == NULL) {
                continue;
            }
            wchar_t *separator = wcsrchr(full, L'\\');
            if (separator == NULL) {
                separator = wcsrchr(full, L'/');
            }
            if (separator != NULL) {
                *separator = L'\0';
                app.rows[index].name = _wcsdup(separator + 1);
                app.rows[index].folder = _wcsdup(full);
            } else {
                app.rows[index].name = _wcsdup(full);
                app.rows[index].folder = _wcsdup(L"");
            }
            app.rows[index].group = (int)g + 1;
            app.rows[index].size = file->size;
            app.rows[index].modified_at = file->modified_at;
            app.rows[index].checked = 0;
            free(full);
            index++;
        }
    }
    app.row_count = index;
}

static size_t checked_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < app.row_count; i++) {
        if (app.rows[i].checked) {
            count++;
        }
    }
    return count;
}

/* ---------- selection helpers ---------- */

/*
 * Check every member of each group except the newest (or oldest) one. The
 * keeper is chosen per group, so at least one copy of every group always
 * stays unchecked — the invariant the Go engine enforces.
 */
static void select_all_except(int keep_newest) {
    for (size_t i = 0; i < app.row_count;) {
        size_t end = i;
        while (end < app.row_count && app.rows[end].group == app.rows[i].group) {
            end++;
        }
        size_t keeper = i;
        for (size_t j = i + 1; j < end; j++) {
            int newer = app.rows[j].modified_at > app.rows[keeper].modified_at;
            int older = app.rows[j].modified_at < app.rows[keeper].modified_at;
            if ((keep_newest && newer) || (!keep_newest && older)) {
                keeper = j;
            }
        }
        for (size_t j = i; j < end; j++) {
            app.rows[j].checked = (j != keeper);
        }
        i = end;
    }
    InvalidateRect(app.list, NULL, TRUE);
}

static void clear_selection(void) {
    for (size_t i = 0; i < app.row_count; i++) {
        app.rows[i].checked = 0;
    }
    InvalidateRect(app.list, NULL, TRUE);
}

/* ---------- worker thread ---------- */

static int worker_progress(void *ctx, const char *stage, size_t done, size_t total) {
    (void)ctx;
    if (InterlockedCompareExchange(&app.cancel_requested, 0, 0) != 0) {
        return 0;
    }
    /* Throttle: posting for every file would flood the message queue. */
    static size_t last_posted;
    if (done != 0 && done > last_posted && done - last_posted < 64 && done != total) {
        return 1;
    }
    last_posted = done;

    progress_message *message = calloc(1, sizeof *message);
    if (message == NULL) {
        return 1;
    }
    wchar_t *wide_stage = widen(stage);
    if (wide_stage != NULL) {
        wcsncpy(message->stage, wide_stage, (sizeof message->stage / sizeof(wchar_t)) - 1);
        free(wide_stage);
    }
    message->done = done;
    message->total = total;
    if (!PostMessageW(app.main, WM_APP_PROGRESS, 0, (LPARAM)message)) {
        free(message);
    }
    return 1;
}

static DWORD WINAPI surface_thread(LPVOID parameter) {
    char *root = parameter;
    td_surface *surface = calloc(1, sizeof *surface);
    int status = TD_ERR_MEMORY;
    if (surface != NULL) {
        td_options options = td_default_options();
        status = td_surface_scan(root, &options, surface, worker_progress, NULL);
    }
    free(root);
    if (status != TD_OK) {
        free(surface);
        surface = NULL;
    }
    PostMessageW(app.main, WM_APP_SURFACE, (WPARAM)status, (LPARAM)surface);
    return 0;
}

static DWORD WINAPI duplicate_thread(LPVOID parameter) {
    td_options *options = parameter;
    td_result *result = calloc(1, sizeof *result);
    int status = TD_ERR_MEMORY;
    if (result != NULL) {
        status = td_find_duplicates(&app.surface, options, result, worker_progress, NULL);
    }
    free(options);
    if (status != TD_OK) {
        free(result);
        result = NULL;
    }
    PostMessageW(app.main, WM_APP_RESULT, (WPARAM)status, (LPARAM)result);
    return 0;
}

/* ---------- UI state ---------- */

static void update_controls(void) {
    BOOL busy = InterlockedCompareExchange(&app.scanning, 0, 0) != 0;
    BOOL has_rows = app.row_count > 0;

    EnableWindow(app.select_button, !busy);
    EnableWindow(app.surface_button, !busy && app.has_folder);
    EnableWindow(app.find_button, !busy && app.has_surface);
    EnableWindow(app.cancel_button, busy);
    for (size_t i = 0; i < TD_CATEGORY_COUNT; i++) {
        EnableWindow(app.category_checks[i], !busy && app.has_surface);
    }
    EnableWindow(app.keep_newest_button, !busy && has_rows);
    EnableWindow(app.keep_oldest_button, !busy && has_rows);
    EnableWindow(app.clear_select_button, !busy && checked_count() > 0);
    EnableWindow(app.export_button, !busy && has_rows);
    EnableWindow(app.explorer_button,
                 !busy && ListView_GetSelectedCount(app.list) > 0);
}

/* Label each category checkbox with the count found by the surface scan, so
 * the user can see where duplicates might be before hashing anything. */
static void refresh_category_labels(void) {
    for (size_t i = 0; i < TD_CATEGORY_COUNT; i++) {
        wchar_t label[64];
        wchar_t *wide_name = widen(td_category_label((td_category)i));
        size_t files = app.has_surface ? app.surface.category_stats[i].files : 0;
        _snwprintf(label, sizeof label / sizeof *label, L"%s (%zu)",
                   wide_name ? wide_name : L"?", files);
        free(wide_name);
        SetWindowTextW(app.category_checks[i], label);
        /* Preselect only categories that actually contain files. */
        SendMessageW(app.category_checks[i], BM_SETCHECK,
                     files > 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

static void collect_selected_categories(td_options *options) {
    for (size_t i = 0; i < TD_CATEGORY_COUNT; i++) {
        options->categories[i] =
            SendMessageW(app.category_checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
}

/* ---------- actions ---------- */

static void choose_folder(void) {
    BROWSEINFOW info;
    memset(&info, 0, sizeof info);
    info.hwndOwner = app.main;
    info.lpszTitle = L"Select a folder to scan for duplicate files";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST selected = SHBrowseForFolderW(&info);
    if (selected == NULL) {
        return;
    }
    wchar_t chosen[MAX_PATH];
    if (SHGetPathFromIDListW(selected, chosen)) {
        wcsncpy(app.folder, chosen, MAX_PATH - 1);
        app.folder[MAX_PATH - 1] = L'\0';
        app.has_folder = 1;
        SetWindowTextW(app.path_label, app.folder);
        clear_surface();
        refresh_category_labels();
        set_status(L"Folder selected. Choose Surface Scan to inventory user files.");
    }
    CoTaskMemFree(selected);
    update_controls();
}

static void begin_operation(const wchar_t *status) {
    InterlockedExchange(&app.cancel_requested, 0);
    InterlockedExchange(&app.scanning, 1);
    app.started_at = GetTickCount64();
    SetTimer(app.main, ID_ELAPSED_TIMER, 250, NULL);
    SendMessageW(app.progress, PBM_SETMARQUEE, TRUE, 30);
    set_status(status);
    update_controls();
}

static void end_operation(void) {
    InterlockedExchange(&app.scanning, 0);
    KillTimer(app.main, ID_ELAPSED_TIMER);
    if (app.thread != NULL) {
        CloseHandle(app.thread);
        app.thread = NULL;
    }
    SendMessageW(app.progress, PBM_SETMARQUEE, FALSE, 0);
    SendMessageW(app.progress, PBM_SETPOS, 0, 0);
    update_controls();
}

static void start_surface_scan(void) {
    if (!app.has_folder || InterlockedCompareExchange(&app.scanning, 0, 0) != 0) {
        return;
    }
    clear_surface();
    refresh_category_labels();

    char *root = calloc(MAX_PATH * 4, 1);
    if (root == NULL) {
        return;
    }
    narrow_into(app.folder, root, MAX_PATH * 4);

    begin_operation(L"Surface scanning: inventorying user files...");
    app.thread = CreateThread(NULL, 0, surface_thread, root, 0, NULL);
    if (app.thread == NULL) {
        free(root);
        end_operation();
        set_status(L"Could not start the scan thread.");
    }
}

static void start_duplicate_scan(void) {
    if (!app.has_surface || InterlockedCompareExchange(&app.scanning, 0, 0) != 0) {
        return;
    }
    clear_result();

    td_options *options = calloc(1, sizeof *options);
    if (options == NULL) {
        return;
    }
    *options = td_default_options();
    collect_selected_categories(options);

    begin_operation(L"Finding duplicates in the selected file types...");
    app.thread = CreateThread(NULL, 0, duplicate_thread, options, 0, NULL);
    if (app.thread == NULL) {
        free(options);
        end_operation();
        set_status(L"Could not start the scan thread.");
    }
}

static void request_cancel(void) {
    if (InterlockedCompareExchange(&app.scanning, 0, 0) == 0) {
        return;
    }
    InterlockedExchange(&app.cancel_requested, 1);
    set_status(L"Cancelling...");
}

static void show_in_explorer(void) {
    int index = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
    if (index < 0 || (size_t)index >= app.row_count) {
        return;
    }
    const row *entry = &app.rows[index];
    size_t length = wcslen(entry->folder) + wcslen(entry->name) + 16;
    wchar_t *argument = calloc(length, sizeof *argument);
    if (argument == NULL) {
        return;
    }
    /* /select, highlights the file in Explorer without opening it. */
    _snwprintf(argument, length, L"/select,\"%s\\%s\"", entry->folder, entry->name);
    ShellExecuteW(app.main, L"open", L"explorer.exe", argument, NULL, SW_SHOWNORMAL);
    free(argument);
}

static void export_report(void) {
    if (!app.has_result || app.result.count == 0) {
        return;
    }
    wchar_t path[MAX_PATH] = L"twintidy-duplicates.csv";
    OPENFILENAMEW dialog;
    memset(&dialog, 0, sizeof dialog);
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = app.main;
    dialog.lpstrFilter = L"CSV report\0*.csv\0JSON report\0*.json\0";
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = L"csv";

    if (!GetSaveFileNameW(&dialog)) {
        return;
    }
    td_report_format format = dialog.nFilterIndex == 2 ? TD_REPORT_JSON : TD_REPORT_CSV;

    char narrow_path[MAX_PATH * 4];
    char narrow_folder[MAX_PATH * 4];
    narrow_into(path, narrow_path, sizeof narrow_path);
    narrow_into(app.folder, narrow_folder, sizeof narrow_folder);

    if (td_report_write_file(narrow_path, format, narrow_folder, &app.result)) {
        wchar_t message[MAX_PATH + 64];
        _snwprintf(message, sizeof message / sizeof *message, L"Report saved to %s", path);
        set_status(message);
    } else {
        MessageBoxW(app.main,
                    L"The report could not be written. No existing file was replaced.",
                    L"Export Failed", MB_OK | MB_ICONWARNING);
    }
}

/* ---------- message handlers ---------- */

static void on_progress(progress_message *message) {
    if (message == NULL) {
        return;
    }
    SetWindowTextW(app.stage_label, message->stage);
    if (message->total > 0) {
        SendMessageW(app.progress, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(app.progress, GWL_STYLE,
                          GetWindowLongPtrW(app.progress, GWL_STYLE) & ~PBS_MARQUEE);
        SendMessageW(app.progress, PBM_SETRANGE32, 0, (LPARAM)message->total);
        SendMessageW(app.progress, PBM_SETPOS, (WPARAM)message->done, 0);
    }
    free(message);
}

static void on_surface_finished(int status, td_surface *surface) {
    end_operation();
    if (status == TD_CANCELLED) {
        set_status(L"Surface scan cancelled. No files were changed.");
        free(surface);
        return;
    }
    if (status == TD_ERR_ROOT) {
        set_status(L"That folder is a protected system or build location and cannot be scanned.");
        free(surface);
        return;
    }
    if (status != TD_OK || surface == NULL) {
        set_status(L"Surface scan failed. No files were changed.");
        free(surface);
        return;
    }

    app.surface = *surface;
    app.has_surface = 1;
    free(surface); /* the contents now belong to app.surface */
    refresh_category_labels();

    wchar_t total[64];
    format_bytes(app.surface.total_bytes, total, sizeof total / sizeof *total);
    wchar_t text[320];
    _snwprintf(text, sizeof text / sizeof *text,
               L"%zu user file(s), %s across %zu folder(s). %zu protected item(s) skipped. "
               L"Choose file types, then Find Duplicates.",
               app.surface.count, total, app.surface.directories_scanned,
               app.surface.skipped_system_items);
    set_status(text);
    update_controls();
}

static void on_result_finished(int status, td_result *result) {
    end_operation();
    if (status == TD_CANCELLED) {
        set_status(L"Duplicate scan cancelled. The surface inventory remains available.");
        free(result);
        return;
    }
    if (status != TD_OK || result == NULL) {
        set_status(L"Duplicate scan failed. No files were changed.");
        free(result);
        return;
    }

    app.result = *result;
    app.has_result = 1;
    free(result);
    build_rows();
    ListView_SetItemCountEx(app.list, (int)app.row_count, LVSICF_NOSCROLL);
    InvalidateRect(app.list, NULL, TRUE);

    wchar_t reclaimable[64];
    format_bytes(app.result.reclaimable, reclaimable,
                 sizeof reclaimable / sizeof *reclaimable);
    wchar_t text[320];
    if (app.result.count == 0) {
        _snwprintf(text, sizeof text / sizeof *text,
                   L"No duplicates found among %zu file(s) in the selected types. "
                   L"Change the file-type focus and choose Find Duplicates again.",
                   app.result.files_considered);
    } else {
        _snwprintf(text, sizeof text / sizeof *text,
                   L"%zu duplicate group(s) across %zu file(s). Keeping one copy of each "
                   L"would reclaim %s.",
                   app.result.count, app.row_count, reclaimable);
    }
    set_status(text);
    update_controls();
}

/* Virtual list view: fill only the cells Windows asks for, and report
 * checkbox state from our own model since owner-data views store none. */
static void on_get_display_info(NMLVDISPINFOW *info) {
    int index = info->item.iItem;
    if (index < 0 || (size_t)index >= app.row_count) {
        return;
    }
    const row *entry = &app.rows[index];
    static wchar_t buffer[64];

    if (info->item.mask & LVIF_STATE) {
        info->item.stateMask = LVIS_STATEIMAGEMASK;
        info->item.state = INDEXTOSTATEIMAGEMASK(entry->checked ? 2 : 1);
    }
    if ((info->item.mask & LVIF_TEXT) == 0) {
        return;
    }
    switch (info->item.iSubItem) {
    case 0:
        _snwprintf(buffer, sizeof buffer / sizeof *buffer, L"%d", entry->group);
        info->item.pszText = buffer;
        break;
    case 1:
        format_bytes(entry->size, buffer, sizeof buffer / sizeof *buffer);
        info->item.pszText = buffer;
        break;
    case 2:
        info->item.pszText = entry->name ? entry->name : L"";
        break;
    case 3:
        format_local_time(entry->modified_at, buffer, sizeof buffer / sizeof *buffer);
        info->item.pszText = buffer;
        break;
    case 4:
        info->item.pszText = entry->folder ? entry->folder : L"";
        break;
    default:
        break;
    }
}

/* Owner-data views do not toggle checkboxes themselves; hit-test the click
 * against the state icon and update our model. */
static void on_list_click(void) {
    LVHITTESTINFO hit;
    memset(&hit, 0, sizeof hit);
    GetCursorPos(&hit.pt);
    ScreenToClient(app.list, &hit.pt);
    ListView_HitTest(app.list, &hit);
    if (hit.iItem < 0 || (size_t)hit.iItem >= app.row_count) {
        return;
    }
    if (hit.flags & LVHT_ONITEMSTATEICON) {
        app.rows[hit.iItem].checked = !app.rows[hit.iItem].checked;
        ListView_RedrawItems(app.list, hit.iItem, hit.iItem);
    }
    update_controls();
}

/* ---------- layout ---------- */

static void layout(int width, int height) {
    const int margin = 12;
    const int line = 28;
    const int button = 130;

    int y = margin;
    MoveWindow(app.select_button, margin, y, button, line, TRUE);
    MoveWindow(app.path_label, margin * 2 + button, y + 4,
               width - (margin * 3 + button), line, TRUE);

    y += line + 8;
    MoveWindow(app.focus_label, margin, y, width - margin * 2, 18, TRUE);
    y += 20;

    /* Category checkboxes wrap across the available width. */
    const int check_width = 132;
    int column = 0;
    for (size_t i = 0; i < TD_CATEGORY_COUNT; i++) {
        int x = margin + column * check_width;
        if (x + check_width > width - margin && column > 0) {
            column = 0;
            x = margin;
            y += 24;
        }
        MoveWindow(app.category_checks[i], x, y, check_width - 6, 22, TRUE);
        column++;
    }

    y += 32;
    MoveWindow(app.surface_button, margin, y, button, line, TRUE);
    MoveWindow(app.find_button, margin * 2 + button, y, button, line, TRUE);
    MoveWindow(app.cancel_button, margin * 3 + button * 2, y, button, line, TRUE);
    int progress_width = width - (margin * 5 + button * 3 + 180);
    if (progress_width < 60) {
        progress_width = 60;
    }
    MoveWindow(app.progress, margin * 4 + button * 3, y + 4, progress_width, line - 8, TRUE);
    MoveWindow(app.stage_label, width - margin - 176, y + 6, 110, 18, TRUE);
    MoveWindow(app.elapsed_label, width - margin - 62, y + 6, 62, 18, TRUE);

    y += line + margin;
    MoveWindow(app.keep_newest_button, margin, y, button, line, TRUE);
    MoveWindow(app.keep_oldest_button, margin * 2 + button, y, button, line, TRUE);
    MoveWindow(app.clear_select_button, margin * 3 + button * 2, y, button, line, TRUE);
    MoveWindow(app.export_button, margin * 4 + button * 3, y, button, line, TRUE);
    MoveWindow(app.explorer_button, margin * 5 + button * 4, y, button + 20, line, TRUE);

    y += line + margin;
    const int status_height = 34;
    int list_height = height - y - status_height - margin;
    if (list_height < 60) {
        list_height = 60;
    }
    MoveWindow(app.list, margin, y, width - margin * 2, list_height, TRUE);
    MoveWindow(app.status, margin, y + list_height + 6, width - margin * 2,
               status_height - 6, TRUE);
}

static void create_controls(HWND window) {
    app.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    const struct {
        HWND *target;
        const wchar_t *class_name;
        const wchar_t *text;
        DWORD style;
        int id;
    } controls[] = {
        {&app.select_button, L"BUTTON", L"Select Folder", BS_PUSHBUTTON, ID_SELECT_FOLDER},
        {&app.path_label, L"STATIC", L"No folder selected.", SS_PATHELLIPSIS, 0},
        {&app.focus_label, L"STATIC",
         L"File-type focus (counts come from the surface scan):", 0, 0},
        {&app.surface_button, L"BUTTON", L"Surface Scan", BS_PUSHBUTTON, ID_SURFACE_SCAN},
        {&app.find_button, L"BUTTON", L"Find Duplicates", BS_DEFPUSHBUTTON, ID_FIND},
        {&app.cancel_button, L"BUTTON", L"Cancel", BS_PUSHBUTTON, ID_CANCEL},
        {&app.stage_label, L"STATIC", L"", SS_RIGHT, 0},
        {&app.elapsed_label, L"STATIC", L"", SS_RIGHT, 0},
        {&app.keep_newest_button, L"BUTTON", L"Keep Newest", BS_PUSHBUTTON, ID_KEEP_NEWEST},
        {&app.keep_oldest_button, L"BUTTON", L"Keep Oldest", BS_PUSHBUTTON, ID_KEEP_OLDEST},
        {&app.clear_select_button, L"BUTTON", L"Clear Selection", BS_PUSHBUTTON, ID_CLEAR_SELECT},
        {&app.export_button, L"BUTTON", L"Export Report", BS_PUSHBUTTON, ID_EXPORT},
        {&app.explorer_button, L"BUTTON", L"Show In Explorer", BS_PUSHBUTTON, ID_EXPLORER},
    };
    for (size_t i = 0; i < sizeof controls / sizeof *controls; i++) {
        *controls[i].target = CreateWindowExW(
            0, controls[i].class_name, controls[i].text,
            WS_CHILD | WS_VISIBLE | controls[i].style, 0, 0, 0, 0, window,
            (HMENU)(INT_PTR)controls[i].id, NULL, NULL);
        SendMessageW(*controls[i].target, WM_SETFONT, (WPARAM)app.font, TRUE);
    }

    for (size_t i = 0; i < TD_CATEGORY_COUNT; i++) {
        app.category_checks[i] = CreateWindowExW(
            0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0,
            window, (HMENU)(INT_PTR)(ID_CATEGORY_BASE + i), NULL, NULL);
        SendMessageW(app.category_checks[i], WM_SETFONT, (WPARAM)app.font, TRUE);
    }
    refresh_category_labels();

    app.progress = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                                   WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 0, 0, 0, 0,
                                   window, NULL, NULL, NULL);
    app.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA |
                                   LVS_SHOWSELALWAYS | WS_TABSTOP,
                               0, 0, 0, 0, window, (HMENU)ID_LIST, NULL, NULL);
    app.status = CreateWindowExW(0, L"STATIC",
                                 L"Select a folder, run a surface scan, choose file types, "
                                 L"then find duplicates. This window never changes your files.",
                                 WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, NULL, NULL, NULL);
    SendMessageW(app.list, WM_SETFONT, (WPARAM)app.font, TRUE);
    SendMessageW(app.status, WM_SETFONT, (WPARAM)app.font, TRUE);

    ListView_SetExtendedListViewStyle(
        app.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);

    LVCOLUMNW column;
    memset(&column, 0, sizeof column);
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    const struct {
        const wchar_t *title;
        int width;
    } columns[] = {
        {L"Group", 70}, {L"Size", 90}, {L"File", 260}, {L"Modified", 130}, {L"Folder", 420},
    };
    for (int i = 0; i < 5; i++) {
        column.iSubItem = i;
        column.pszText = (LPWSTR)columns[i].title;
        column.cx = columns[i].width;
        ListView_InsertColumn(app.list, i, &column);
    }
}

static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam,
                                         LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        app.main = window;
        create_controls(window);
        update_controls();
        return 0;

    case WM_SIZE:
        layout(LOWORD(lparam), HIWORD(lparam));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *limits = (MINMAXINFO *)lparam;
        limits->ptMinTrackSize.x = 900;
        limits->ptMinTrackSize.y = 560;
        return 0;
    }

    case WM_TIMER:
        if (wparam == ID_ELAPSED_TIMER) {
            ULONGLONG elapsed = (GetTickCount64() - app.started_at) / 1000;
            wchar_t text[32];
            _snwprintf(text, sizeof text / sizeof *text, L"%llu:%02llu", elapsed / 60,
                       elapsed % 60);
            SetWindowTextW(app.elapsed_label, text);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_SELECT_FOLDER:
            choose_folder();
            return 0;
        case ID_SURFACE_SCAN:
            start_surface_scan();
            return 0;
        case ID_FIND:
            start_duplicate_scan();
            return 0;
        case ID_CANCEL:
            request_cancel();
            return 0;
        case ID_KEEP_NEWEST:
            select_all_except(1);
            update_controls();
            return 0;
        case ID_KEEP_OLDEST:
            select_all_except(0);
            update_controls();
            return 0;
        case ID_CLEAR_SELECT:
            clear_selection();
            update_controls();
            return 0;
        case ID_EXPORT:
            export_report();
            return 0;
        case ID_EXPLORER:
            show_in_explorer();
            return 0;
        default:
            break;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)lparam;
        if (header->idFrom != ID_LIST) {
            break;
        }
        if (header->code == LVN_GETDISPINFOW) {
            on_get_display_info((NMLVDISPINFOW *)lparam);
            return 0;
        }
        if (header->code == NM_CLICK) {
            on_list_click();
            return 0;
        }
        if (header->code == LVN_ITEMCHANGED) {
            update_controls();
            return 0;
        }
        break;
    }

    case WM_APP_PROGRESS:
        on_progress((progress_message *)lparam);
        return 0;

    case WM_APP_SURFACE:
        on_surface_finished((int)wparam, (td_surface *)lparam);
        return 0;

    case WM_APP_RESULT:
        on_result_finished((int)wparam, (td_result *)lparam);
        return 0;

    case WM_CLOSE:
        /* A scan owns a worker thread; ask it to stop and let the completion
         * message arrive before tearing the window down. */
        if (InterlockedCompareExchange(&app.scanning, 0, 0) != 0) {
            request_cancel();
            return 0;
        }
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        clear_surface();
        if (app.font != NULL) {
            DeleteObject(app.font);
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous;
    (void)command_line;

    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX controls;
    controls.dwSize = sizeof controls;
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW class;
    memset(&class, 0, sizeof class);
    class.cbSize = sizeof class;
    class.lpfnWndProc = window_procedure;
    class.hInstance = instance;
    class.hCursor = LoadCursor(NULL, IDC_ARROW);
    class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    class.lpszClassName = L"TwinTidyCPortWindow";
    if (!RegisterClassExW(&class)) {
        return 1;
    }

    HWND window = CreateWindowExW(0, class.lpszClassName,
                                  L"TwinTidy (C port) - Duplicate File Review",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  1180, 720, NULL, NULL, instance, NULL);
    if (window == NULL) {
        return 1;
    }
    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    CoUninitialize();
    return 0;
}
