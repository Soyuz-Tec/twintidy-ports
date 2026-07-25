/*
 * gui.c — native Win32 front end for the TwinTidy C port.
 *
 * Read-only by construction: this window can select a folder, scan it, and
 * review the results. It exposes no action that deletes or modifies a file.
 *
 * The scan runs on a worker thread and reports progress by posting messages
 * to the UI thread, so the window stays responsive and cancellable.
 *
 * Windows only. Build: make gui   (or see the Makefile)
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include "scanner.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_SELECT_FOLDER 1001
#define ID_SCAN          1002
#define ID_CANCEL        1003
#define ID_LIST          1004
#define ID_PROGRESS      1005
#define ID_STATUS        1006
#define ID_PATH          1007

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_FINISHED (WM_APP + 2)

/* One display row: a single file inside a duplicate group. */
typedef struct {
    int group;            /* 1-based group number shown to the user */
    uint64_t size;
    wchar_t *name;
    wchar_t *folder;
} row;

/* Progress posted from the worker thread; the UI thread owns and frees it. */
typedef struct {
    wchar_t stage[64];
    size_t done;
    size_t total;
} progress_message;

typedef struct {
    HWND main;
    HWND select_button;
    HWND scan_button;
    HWND cancel_button;
    HWND list;
    HWND progress;
    HWND status;
    HWND path_label;
    HFONT font;

    wchar_t folder[MAX_PATH];
    int has_folder;

    row *rows;
    size_t row_count;

    HANDLE thread;
    volatile LONG cancel_requested;
    volatile LONG scanning;
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

/* Flatten confirmed groups into display rows, splitting each path into a
 * file name and its containing folder so both columns stay readable. */
static void build_rows(const td_result *result) {
    free_rows();
    size_t total = 0;
    for (size_t g = 0; g < result->count; g++) {
        total += result->groups[g].count;
    }
    if (total == 0) {
        return;
    }
    app.rows = calloc(total, sizeof *app.rows);
    if (app.rows == NULL) {
        return;
    }

    size_t index = 0;
    for (size_t g = 0; g < result->count; g++) {
        for (size_t f = 0; f < result->groups[g].count; f++) {
            wchar_t *full = widen(result->groups[g].files[f].path);
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
            app.rows[index].size = result->groups[g].files[f].size;
            free(full);
            index++;
        }
    }
    app.row_count = index;
}

/* ---------- worker thread ---------- */

static int worker_progress(void *ctx, const char *stage, size_t done, size_t total) {
    (void)ctx;
    if (InterlockedCompareExchange(&app.cancel_requested, 0, 0) != 0) {
        return 0; /* ask the engine to stop */
    }
    /* Throttle: posting for every file would flood the message queue. */
    static size_t last_posted;
    if (done != 0 && done - last_posted < 64 && done != total) {
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

static DWORD WINAPI scan_thread(LPVOID parameter) {
    char *root = parameter;
    td_result *result = calloc(1, sizeof *result);
    int status = TD_ERR_MEMORY;
    if (result != NULL) {
        td_options options = td_default_options();
        status = td_scan(root, &options, result, worker_progress, NULL);
    }
    free(root);

    if (status != TD_OK) {
        free(result);
        result = NULL;
    }
    PostMessageW(app.main, WM_APP_FINISHED, (WPARAM)status, (LPARAM)result);
    return 0;
}

/* ---------- UI ---------- */

static void set_status(const wchar_t *text) {
    SetWindowTextW(app.status, text);
}

static void update_controls(void) {
    BOOL busy = InterlockedCompareExchange(&app.scanning, 0, 0) != 0;
    EnableWindow(app.select_button, !busy);
    EnableWindow(app.scan_button, !busy && app.has_folder);
    EnableWindow(app.cancel_button, busy);
}

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
        set_status(L"Folder selected. Choose Scan to find duplicate files.");
    }
    CoTaskMemFree(selected);
    update_controls();
}

static void start_scan(void) {
    if (!app.has_folder || InterlockedCompareExchange(&app.scanning, 0, 0) != 0) {
        return;
    }
    free_rows();
    ListView_SetItemCountEx(app.list, 0, 0);
    InvalidateRect(app.list, NULL, TRUE);

    char *root = calloc(MAX_PATH * 4, 1);
    if (root == NULL) {
        return;
    }
    narrow_into(app.folder, root, MAX_PATH * 4);

    InterlockedExchange(&app.cancel_requested, 0);
    InterlockedExchange(&app.scanning, 1);
    SendMessageW(app.progress, PBM_SETMARQUEE, TRUE, 30);
    set_status(L"Scanning...");
    update_controls();

    app.thread = CreateThread(NULL, 0, scan_thread, root, 0, NULL);
    if (app.thread == NULL) {
        free(root);
        InterlockedExchange(&app.scanning, 0);
        set_status(L"Could not start the scan thread.");
        update_controls();
    }
}

static void request_cancel(void) {
    if (InterlockedCompareExchange(&app.scanning, 0, 0) == 0) {
        return;
    }
    InterlockedExchange(&app.cancel_requested, 1);
    set_status(L"Cancelling...");
}

static void on_progress(progress_message *message) {
    if (message == NULL) {
        return;
    }
    wchar_t text[160];
    if (message->total > 0) {
        SendMessageW(app.progress, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(app.progress, GWL_STYLE,
                          GetWindowLongPtrW(app.progress, GWL_STYLE) & ~PBS_MARQUEE);
        SendMessageW(app.progress, PBM_SETRANGE32, 0, (LPARAM)message->total);
        SendMessageW(app.progress, PBM_SETPOS, (WPARAM)message->done, 0);
        _snwprintf(text, sizeof text / sizeof *text, L"%s  (%zu of %zu)",
                   message->stage, message->done, message->total);
    } else {
        _snwprintf(text, sizeof text / sizeof *text, L"%s  (%zu file(s) found)",
                   message->stage, message->done);
    }
    set_status(text);
    free(message);
}

static void on_finished(int status, td_result *result) {
    InterlockedExchange(&app.scanning, 0);
    if (app.thread != NULL) {
        CloseHandle(app.thread);
        app.thread = NULL;
    }
    SendMessageW(app.progress, PBM_SETMARQUEE, FALSE, 0);
    SendMessageW(app.progress, PBM_SETPOS, 0, 0);

    if (status == TD_CANCELLED) {
        set_status(L"Scan cancelled. No files were changed.");
        update_controls();
        return;
    }
    if (status != TD_OK || result == NULL) {
        set_status(L"Scan failed. No files were changed.");
        update_controls();
        return;
    }

    build_rows(result);
    ListView_SetItemCountEx(app.list, (int)app.row_count, LVSICF_NOSCROLL);
    InvalidateRect(app.list, NULL, TRUE);

    wchar_t reclaimable[64];
    format_bytes(result->reclaimable, reclaimable, sizeof reclaimable / sizeof *reclaimable);
    wchar_t text[256];
    if (result->count == 0) {
        _snwprintf(text, sizeof text / sizeof *text,
                   L"No duplicates found among %zu scanned file(s).",
                   result->files_considered);
    } else {
        _snwprintf(text, sizeof text / sizeof *text,
                   L"%zu duplicate group(s) across %zu file(s). Keeping one copy of each would reclaim %s.",
                   result->count, app.row_count, reclaimable);
    }
    set_status(text);

    td_result_free(result);
    free(result);
    update_controls();
}

static void layout(int width, int height) {
    const int margin = 12;
    const int row_height = 28;
    const int button_width = 130;

    MoveWindow(app.select_button, margin, margin, button_width, row_height, TRUE);
    MoveWindow(app.path_label, margin * 2 + button_width, margin + 4,
               width - (margin * 3 + button_width), row_height, TRUE);

    int second = margin * 2 + row_height;
    MoveWindow(app.scan_button, margin, second, button_width, row_height, TRUE);
    MoveWindow(app.cancel_button, margin * 2 + button_width, second, button_width, row_height, TRUE);
    MoveWindow(app.progress, margin * 3 + button_width * 2, second + 4,
               width - (margin * 4 + button_width * 2), row_height - 8, TRUE);

    int third = second + row_height + margin;
    int status_height = 20;
    int list_height = height - third - status_height - margin * 2;
    if (list_height < 40) {
        list_height = 40;
    }
    MoveWindow(app.list, margin, third, width - margin * 2, list_height, TRUE);
    MoveWindow(app.status, margin, third + list_height + 8, width - margin * 2, status_height, TRUE);
}

static void create_controls(HWND window) {
    app.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    app.select_button = CreateWindowExW(0, L"BUTTON", L"Select Folder",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        0, 0, 0, 0, window, (HMENU)ID_SELECT_FOLDER, NULL, NULL);
    app.path_label = CreateWindowExW(0, L"STATIC", L"No folder selected.",
                                     WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS,
                                     0, 0, 0, 0, window, (HMENU)ID_PATH, NULL, NULL);
    app.scan_button = CreateWindowExW(0, L"BUTTON", L"Scan",
                                      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                      0, 0, 0, 0, window, (HMENU)ID_SCAN, NULL, NULL);
    app.cancel_button = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        0, 0, 0, 0, window, (HMENU)ID_CANCEL, NULL, NULL);
    app.progress = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                                   WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
                                   0, 0, 0, 0, window, (HMENU)ID_PROGRESS, NULL, NULL);
    app.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA |
                                   LVS_SHOWSELALWAYS | WS_TABSTOP,
                               0, 0, 0, 0, window, (HMENU)ID_LIST, NULL, NULL);
    app.status = CreateWindowExW(0, L"STATIC",
                                 L"Select a folder, then choose Scan. This window never changes your files.",
                                 WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS,
                                 0, 0, 0, 0, window, (HMENU)ID_STATUS, NULL, NULL);

    ListView_SetExtendedListViewStyle(app.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNW column;
    memset(&column, 0, sizeof column);
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    struct { const wchar_t *title; int width; } columns[] = {
        {L"Group", 70},
        {L"Size", 100},
        {L"File", 280},
        {L"Folder", 520},
    };
    for (int i = 0; i < 4; i++) {
        column.iSubItem = i;
        column.pszText = (LPWSTR)columns[i].title;
        column.cx = columns[i].width;
        ListView_InsertColumn(app.list, i, &column);
    }

    HWND controls[] = {app.select_button, app.path_label, app.scan_button,
                       app.cancel_button, app.list, app.status};
    for (size_t i = 0; i < sizeof controls / sizeof *controls; i++) {
        SendMessageW(controls[i], WM_SETFONT, (WPARAM)app.font, TRUE);
    }
}

/* Virtual list view: fill only the cells Windows actually asks for. */
static void on_get_display_info(NMLVDISPINFOW *info) {
    if ((info->item.mask & LVIF_TEXT) == 0) {
        return;
    }
    int index = info->item.iItem;
    if (index < 0 || (size_t)index >= app.row_count) {
        return;
    }
    const row *entry = &app.rows[index];
    static wchar_t buffer[64];
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
        info->item.pszText = entry->folder ? entry->folder : L"";
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
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
        limits->ptMinTrackSize.x = 720;
        limits->ptMinTrackSize.y = 400;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_SELECT_FOLDER:
            choose_folder();
            return 0;
        case ID_SCAN:
            start_scan();
            return 0;
        case ID_CANCEL:
            request_cancel();
            return 0;
        default:
            break;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)lparam;
        if (header->idFrom == ID_LIST && header->code == LVN_GETDISPINFOW) {
            on_get_display_info((NMLVDISPINFOW *)lparam);
            return 0;
        }
        break;
    }

    case WM_APP_PROGRESS:
        on_progress((progress_message *)lparam);
        return 0;

    case WM_APP_FINISHED:
        on_finished((int)wparam, (td_result *)lparam);
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
        free_rows();
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
                                  1000, 620, NULL, NULL, instance, NULL);
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
