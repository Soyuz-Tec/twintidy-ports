//! Native Win32 front end for the TwinTidy Rust port.
//!
//! Read-only by construction: this window selects a folder, runs the
//! two-phase scan, and reviews the results. It exposes no action that
//! deletes, moves, or modifies a file; checkbox selection exists so a user
//! can plan cleanup and export the plan, not perform it.
//!
//! Scans run on a worker thread and report progress by posting messages to
//! the UI thread, so the window stays responsive and cancellable.
//!
//! Windows only; on other targets this binary is an explanatory stub.

#![cfg_attr(windows, windows_subsystem = "windows")]

#[cfg(not(windows))]
fn main() {
    eprintln!("twintidy-gui is a Windows-only front end; use the twintidy CLI on this platform.");
}

#[cfg(windows)]
fn main() {
    windows_gui::run();
}

#[cfg(windows)]
mod windows_gui {
    use std::ffi::c_void;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::time::{SystemTime, UNIX_EPOCH};

    use twintidy::report::{self, Format};
    use twintidy::settings::{self, Settings};
    use twintidy::{
        find_duplicates, format_bytes, format_timestamp, surface_scan, Category, Flow, Options,
        ScanError, ScanResult, Stage, SurfaceReport,
    };

    use windows_sys::core::{w, PCWSTR};
    use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, WPARAM};
    use windows_sys::Win32::Graphics::Gdi::{
        CreateFontW, DeleteObject, GetDC, GetDeviceCaps, InvalidateRect, ReleaseDC, ScreenToClient,
        UpdateWindow, CLEARTYPE_QUALITY, CLIP_DEFAULT_PRECIS, COLOR_BTNFACE, DEFAULT_CHARSET,
        DEFAULT_PITCH, FF_DONTCARE, FW_NORMAL, HFONT, LOGPIXELSX, OUT_DEFAULT_PRECIS,
    };
    use windows_sys::Win32::System::Com::{
        CoInitializeEx, CoUninitialize, COINIT_APARTMENTTHREADED,
    };
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::System::SystemInformation::GetTickCount64;
    use windows_sys::Win32::System::SystemServices::{SS_PATHELLIPSIS, SS_RIGHT};
    use windows_sys::Win32::UI::Controls::Dialogs::{
        GetSaveFileNameW, OFN_NOCHANGEDIR, OFN_OVERWRITEPROMPT, OFN_PATHMUSTEXIST, OPENFILENAMEW,
    };
    use windows_sys::Win32::UI::Controls::{
        InitCommonControlsEx, BST_CHECKED, BST_UNCHECKED, INITCOMMONCONTROLSEX, LVCFMT_LEFT,
        LVCF_SUBITEM, LVCF_TEXT, LVCF_WIDTH, LVCOLUMNW, LVHITTESTINFO, LVHT_ONITEMSTATEICON,
        LVIF_STATE, LVIF_TEXT, LVIS_STATEIMAGEMASK, LVM_GETNEXTITEM, LVM_GETSELECTEDCOUNT,
        LVM_HITTEST, LVM_INSERTCOLUMNW, LVM_REDRAWITEMS, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVM_SETITEMCOUNT, LVNI_SELECTED, LVN_GETDISPINFOW, LVN_ITEMCHANGED, LVS_EX_CHECKBOXES,
        LVS_EX_FULLROWSELECT, LVS_EX_GRIDLINES, LVS_OWNERDATA, LVS_REPORT, LVS_SHOWSELALWAYS,
        NMHDR, NMLVDISPINFOW, NM_CLICK, PBM_SETMARQUEE, PBM_SETPOS, PBM_SETRANGE32, PBS_MARQUEE,
        WC_LISTVIEWW,
    };
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::EnableWindow;
    use windows_sys::Win32::UI::Shell::{
        SHBrowseForFolderW, SHGetPathFromIDListW, ShellExecuteW, BIF_NEWDIALOGSTYLE,
        BIF_RETURNONLYFSDIRS, BROWSEINFOW,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::*;

    const ID_SELECT: usize = 1001;
    const ID_SURFACE: usize = 1002;
    const ID_FIND: usize = 1003;
    const ID_CANCEL: usize = 1004;
    const ID_LIST: usize = 1005;
    const ID_KEEP_NEWEST: usize = 1006;
    const ID_KEEP_OLDEST: usize = 1007;
    const ID_CLEAR_SELECT: usize = 1008;
    const ID_EXPORT: usize = 1009;
    const ID_EXPLORER: usize = 1010;
    const ID_CLEAR_RESULTS: usize = 1011;
    const ID_RESET: usize = 1012;
    const ID_ABOUT: usize = 1013;
    const ID_PREVIEW_SAFETY: usize = 1014;
    const ID_CATEGORY_BASE: usize = 1100;

    const WM_APP_PROGRESS: u32 = WM_APP + 1;
    const WM_APP_SURFACE: u32 = WM_APP + 2;
    const WM_APP_RESULT: u32 = WM_APP + 3;
    const ID_ELAPSED_TIMER: usize = 1;

    /// One display row: a single file inside a duplicate group.
    struct Row {
        group: usize,
        size: u64,
        modified_at: i64,
        checked: bool,
        name: Vec<u16>,
        folder: Vec<u16>,
        /// Category label.
        kind: Vec<u16>,
        /// Abbreviated SHA-256 of the group; the full digest is in the report.
        hash: Vec<u16>,
        full_path: PathBuf,
    }

    /// Progress posted from the worker thread; the UI thread takes ownership.
    struct ProgressMessage {
        stage: &'static str,
        done: usize,
        total: usize,
    }

    #[derive(Default)]
    struct App {
        main: HWND,
        select_button: HWND,
        surface_button: HWND,
        find_button: HWND,
        cancel_button: HWND,
        keep_newest_button: HWND,
        keep_oldest_button: HWND,
        clear_select_button: HWND,
        export_button: HWND,
        explorer_button: HWND,
        clear_results_button: HWND,
        reset_button: HWND,
        about_button: HWND,
        preview_safety_button: HWND,
        files_label: HWND,
        current_label: HWND,
        list: HWND,
        progress: HWND,
        status: HWND,
        path_label: HWND,
        stage_label: HWND,
        elapsed_label: HWND,
        focus_label: HWND,
        category_checks: Vec<HWND>,
        font: HFONT,

        folder: Option<PathBuf>,
        surface: Option<SurfaceReport>,
        result: Option<ScanResult>,
        rows: Vec<Row>,

        scanning: bool,
        cancel_flag: Option<Arc<AtomicBool>>,
        started_at: u64,

        settings_path: Option<PathBuf>,

        /// Screen DPI. The process declares itself DPI-aware, so Windows does
        /// not scale it; every layout metric is expressed at 96 DPI and scaled
        /// here instead. Without this, controls render at a fraction of their
        /// intended size on a high-DPI display.
        dpi: i32,
    }

    // The Win32 message loop is single-threaded; every access below happens
    // on the UI thread, inside the window procedure.
    static mut APP: Option<Box<App>> = None;

    #[allow(static_mut_refs)]
    fn app() -> &'static mut App {
        unsafe { APP.as_mut().expect("app state initialized in WM_CREATE") }
    }

    fn wide(text: &str) -> Vec<u16> {
        text.encode_utf16().chain(std::iter::once(0)).collect()
    }

    fn from_wide(buffer: &[u16]) -> String {
        let end = buffer.iter().position(|&c| c == 0).unwrap_or(buffer.len());
        String::from_utf16_lossy(&buffer[..end])
    }

    fn set_text(window: HWND, text: &str) {
        unsafe { SetWindowTextW(window, wide(text).as_ptr()) };
    }

    /// Scale a 96-DPI design metric to the current display.
    fn scaled(value: i32) -> i32 {
        let dpi = app().dpi;
        value * if dpi > 0 { dpi } else { 96 } / 96
    }

    /// Read the DPI of the display a window is on, falling back to the desktop.
    unsafe fn display_dpi(window: HWND) -> i32 {
        let dc = GetDC(window);
        if dc.is_null() {
            return 96;
        }
        let dpi = GetDeviceCaps(dc, LOGPIXELSX as i32);
        ReleaseDC(window, dc);
        if dpi > 0 {
            dpi
        } else {
            96
        }
    }

    fn now_unix() -> i64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|value| value.as_secs() as i64)
            .unwrap_or(0)
    }

    pub fn run() {
        unsafe {
            SetProcessDPIAware();
            CoInitializeEx(std::ptr::null(), COINIT_APARTMENTTHREADED as u32);

            let controls = INITCOMMONCONTROLSEX {
                dwSize: std::mem::size_of::<INITCOMMONCONTROLSEX>() as u32,
                dwICC: 0x0000_0001 | 0x0000_0010 | 0x0000_4000, // listview | progress | standard
            };
            InitCommonControlsEx(&controls);

            APP = Some(Box::default());

            let instance = GetModuleHandleW(std::ptr::null());
            let class_name = w!("TwinTidyRustPortWindow");

            let class = WNDCLASSEXW {
                cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
                style: 0,
                lpfnWndProc: Some(window_procedure),
                cbClsExtra: 0,
                cbWndExtra: 0,
                hInstance: instance,
                hIcon: std::ptr::null_mut(),
                hCursor: LoadCursorW(std::ptr::null_mut(), IDC_ARROW),
                hbrBackground: (COLOR_BTNFACE + 1) as isize as *mut c_void,
                lpszMenuName: std::ptr::null(),
                lpszClassName: class_name,
                hIconSm: std::ptr::null_mut(),
            };
            if RegisterClassExW(&class) == 0 {
                return;
            }

            // Created at a scaled size so it opens proportionate on a
            // high-DPI display; WM_CREATE then reads its own monitor's DPI.
            app().dpi = display_dpi(std::ptr::null_mut());
            let window = CreateWindowExW(
                0,
                class_name,
                w!("TwinTidy (Rust port) - Duplicate File Review"),
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                scaled(1180),
                scaled(720),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                instance,
                std::ptr::null(),
            );
            if window.is_null() {
                return;
            }
            ShowWindow(window, SW_SHOW);
            UpdateWindow(window);

            let mut message: MSG = std::mem::zeroed();
            while GetMessageW(&mut message, std::ptr::null_mut(), 0, 0) > 0 {
                if IsDialogMessageW(window, &message) == 0 {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            CoUninitialize();
        }
    }

    // ---------- construction ----------

    unsafe fn create_controls(window: HWND) {
        let state = app();
        state.main = window;
        state.font = CreateFontW(
            -scaled(15),
            0,
            0,
            0,
            FW_NORMAL as i32,
            0,
            0,
            0,
            DEFAULT_CHARSET as u32,
            OUT_DEFAULT_PRECIS as u32,
            CLIP_DEFAULT_PRECIS as u32,
            CLEARTYPE_QUALITY as u32,
            (DEFAULT_PITCH as u32) | (FF_DONTCARE as u32),
            w!("Segoe UI"),
        );

        let instance = GetModuleHandleW(std::ptr::null());
        let font = state.font;
        let child = |class: PCWSTR, text: PCWSTR, style: u32, id: usize, ex: u32| -> HWND {
            let handle = CreateWindowExW(
                ex,
                class,
                text,
                WS_CHILD | WS_VISIBLE | style,
                0,
                0,
                0,
                0,
                window,
                id as *mut c_void,
                instance,
                std::ptr::null(),
            );
            SendMessageW(handle, WM_SETFONT, font as usize, 1);
            handle
        };

        state.select_button = child(
            w!("BUTTON"),
            w!("Select Folder"),
            BS_PUSHBUTTON as u32,
            ID_SELECT,
            0,
        );
        state.path_label = child(
            w!("STATIC"),
            w!("No folder selected."),
            SS_PATHELLIPSIS,
            0,
            0,
        );
        state.focus_label = child(
            w!("STATIC"),
            w!("File-type focus (counts come from the surface scan):"),
            0,
            0,
            0,
        );
        state.surface_button = child(
            w!("BUTTON"),
            w!("Surface Scan"),
            BS_PUSHBUTTON as u32,
            ID_SURFACE,
            0,
        );
        state.find_button = child(
            w!("BUTTON"),
            w!("Find Duplicates"),
            BS_DEFPUSHBUTTON as u32,
            ID_FIND,
            0,
        );
        state.cancel_button = child(
            w!("BUTTON"),
            w!("Cancel"),
            BS_PUSHBUTTON as u32,
            ID_CANCEL,
            0,
        );
        state.stage_label = child(w!("STATIC"), w!(""), SS_RIGHT, 0, 0);
        state.elapsed_label = child(w!("STATIC"), w!(""), SS_RIGHT, 0, 0);
        state.keep_newest_button = child(
            w!("BUTTON"),
            w!("Keep Newest"),
            BS_PUSHBUTTON as u32,
            ID_KEEP_NEWEST,
            0,
        );
        state.keep_oldest_button = child(
            w!("BUTTON"),
            w!("Keep Oldest"),
            BS_PUSHBUTTON as u32,
            ID_KEEP_OLDEST,
            0,
        );
        state.clear_select_button = child(
            w!("BUTTON"),
            w!("Clear Selection"),
            BS_PUSHBUTTON as u32,
            ID_CLEAR_SELECT,
            0,
        );
        state.export_button = child(
            w!("BUTTON"),
            w!("Export Report"),
            BS_PUSHBUTTON as u32,
            ID_EXPORT,
            0,
        );
        state.explorer_button = child(
            w!("BUTTON"),
            w!("Show In Explorer"),
            BS_PUSHBUTTON as u32,
            ID_EXPLORER,
            0,
        );
        state.clear_results_button = child(
            w!("BUTTON"),
            w!("Clear Results"),
            BS_PUSHBUTTON as u32,
            ID_CLEAR_RESULTS,
            0,
        );
        state.reset_button = child(w!("BUTTON"), w!("Reset"), BS_PUSHBUTTON as u32, ID_RESET, 0);
        state.preview_safety_button = child(
            w!("BUTTON"),
            w!("Preview Safety"),
            BS_PUSHBUTTON as u32,
            ID_PREVIEW_SAFETY,
            0,
        );
        state.about_button = child(w!("BUTTON"), w!("About"), BS_PUSHBUTTON as u32, ID_ABOUT, 0);
        state.files_label = child(w!("STATIC"), w!(""), 0, 0, 0);
        state.current_label = child(w!("STATIC"), w!(""), SS_PATHELLIPSIS, 0, 0);

        state.category_checks = (0..Category::ALL.len())
            .map(|index| {
                child(
                    w!("BUTTON"),
                    w!(""),
                    BS_AUTOCHECKBOX as u32,
                    ID_CATEGORY_BASE + index,
                    0,
                )
            })
            .collect();

        state.progress = child(w!("msctls_progress32"), std::ptr::null(), PBS_MARQUEE, 0, 0);
        state.list = child(
            WC_LISTVIEWW,
            std::ptr::null(),
            LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP,
            ID_LIST,
            WS_EX_CLIENTEDGE,
        );
        state.status = child(
            w!("STATIC"),
            w!("Select a folder, run a surface scan, choose file types, then find duplicates. This window never changes your files."),
            0,
            0,
            0,
        );

        SendMessageW(
            state.list,
            LVM_SETEXTENDEDLISTVIEWSTYLE,
            0,
            (LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES) as isize,
        );

        for (index, (title, width)) in [
            ("No.", 55),
            ("Group", 62),
            ("Name", 230),
            ("Size", 90),
            ("Type", 95),
            ("Modified", 140),
            ("Hash", 130),
            ("Folder", 380),
        ]
        .iter()
        .enumerate()
        {
            let mut heading = wide(title);
            let column = LVCOLUMNW {
                mask: LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM,
                fmt: LVCFMT_LEFT,
                cx: scaled(*width),
                pszText: heading.as_mut_ptr(),
                cchTextMax: 0,
                iSubItem: index as i32,
                iImage: 0,
                iOrder: 0,
                cxMin: 0,
                cxDefault: 0,
                cxIdeal: 0,
            };
            SendMessageW(
                state.list,
                LVM_INSERTCOLUMNW,
                index,
                &column as *const _ as isize,
            );
        }
        refresh_category_labels();
    }

    // ---------- state ----------

    unsafe fn clear_result() {
        let state = app();
        state.rows.clear();
        state.result = None;
        SendMessageW(state.list, LVM_SETITEMCOUNT, 0, 0);
        InvalidateRect(state.list, std::ptr::null(), 1);
    }

    unsafe fn clear_surface() {
        clear_result();
        app().surface = None;
    }

    unsafe fn update_controls() {
        let state = app();
        let busy = state.scanning;
        let has_rows = !state.rows.is_empty();
        let has_checked = state.rows.iter().any(|row| row.checked);
        let has_selection = SendMessageW(state.list, LVM_GETSELECTEDCOUNT, 0, 0) > 0;

        EnableWindow(state.select_button, (!busy) as i32);
        EnableWindow(
            state.surface_button,
            (!busy && state.folder.is_some()) as i32,
        );
        EnableWindow(state.find_button, (!busy && state.surface.is_some()) as i32);
        EnableWindow(state.cancel_button, busy as i32);
        for check in &state.category_checks {
            EnableWindow(*check, (!busy && state.surface.is_some()) as i32);
        }
        EnableWindow(state.keep_newest_button, (!busy && has_rows) as i32);
        EnableWindow(state.keep_oldest_button, (!busy && has_rows) as i32);
        EnableWindow(state.clear_select_button, (!busy && has_checked) as i32);
        EnableWindow(state.export_button, (!busy && has_rows) as i32);
        EnableWindow(state.explorer_button, (!busy && has_selection) as i32);
        EnableWindow(state.clear_results_button, (!busy && has_rows) as i32);
        EnableWindow(
            state.reset_button,
            (!busy && (state.folder.is_some() || state.surface.is_some())) as i32,
        );
        // About and Preview Safety stay available: they only show text.
    }

    /// Label each category checkbox with the count found by the surface scan,
    /// so the user can see where duplicates might be before hashing anything.
    unsafe fn refresh_category_labels() {
        let state = app();
        for (index, category) in Category::ALL.iter().enumerate() {
            let files = state
                .surface
                .as_ref()
                .map(|surface| surface.stats_for(*category).files)
                .unwrap_or(0);
            set_text(
                state.category_checks[index],
                &format!("{} ({})", category.label(), files),
            );
            // Preselect only categories that actually contain files.
            SendMessageW(
                state.category_checks[index],
                BM_SETCHECK,
                if files > 0 {
                    BST_CHECKED as usize
                } else {
                    BST_UNCHECKED as usize
                },
                0,
            );
        }
    }

    unsafe fn selected_categories() -> Vec<Category> {
        let state = app();
        Category::ALL
            .iter()
            .enumerate()
            .filter(|(index, _)| {
                SendMessageW(state.category_checks[*index], BM_GETCHECK, 0, 0)
                    == BST_CHECKED as isize
            })
            .map(|(_, category)| *category)
            .collect()
    }

    /// Split each path into file name and containing folder so both columns
    /// stay readable.
    fn build_rows(result: &ScanResult) -> Vec<Row> {
        let mut rows = Vec::new();
        for (index, group) in result.groups.iter().enumerate() {
            for file in &group.files {
                let name = file
                    .path
                    .file_name()
                    .map(|value| value.to_string_lossy().into_owned())
                    .unwrap_or_default();
                let folder = file
                    .path
                    .parent()
                    .map(|value| value.to_string_lossy().into_owned())
                    .unwrap_or_default();
                // Show a short hash prefix: the full 64 characters are in the
                // exported report, but a column that wide is unreadable.
                let short_hash: String = group.hash.chars().take(16).collect();
                rows.push(Row {
                    group: index + 1,
                    size: file.size,
                    modified_at: file.modified_at,
                    checked: false,
                    name: wide(&name),
                    folder: wide(&folder),
                    kind: wide(file.category.label()),
                    hash: wide(&short_hash),
                    full_path: file.path.clone(),
                });
            }
        }
        rows
    }

    // ---------- selection ----------

    /// Check every member of each group except the newest (or oldest) one.
    /// The keeper is chosen per group, so at least one copy of every group
    /// always stays unchecked — the invariant the Go engine enforces.
    unsafe fn select_all_except(keep_newest: bool) {
        let state = app();
        let mut start = 0;
        while start < state.rows.len() {
            let mut end = start;
            while end < state.rows.len() && state.rows[end].group == state.rows[start].group {
                end += 1;
            }
            let mut keeper = start;
            for index in (start + 1)..end {
                let candidate = state.rows[index].modified_at;
                let current = state.rows[keeper].modified_at;
                if (keep_newest && candidate > current) || (!keep_newest && candidate < current) {
                    keeper = index;
                }
            }
            for (index, row) in state.rows[start..end].iter_mut().enumerate() {
                row.checked = start + index != keeper;
            }
            start = end;
        }
        InvalidateRect(state.list, std::ptr::null(), 1);
        update_controls();
    }

    unsafe fn clear_selection() {
        let state = app();
        for row in &mut state.rows {
            row.checked = false;
        }
        InvalidateRect(state.list, std::ptr::null(), 1);
        update_controls();
    }

    // ---------- actions ----------

    unsafe fn choose_folder() {
        let title = wide("Select a folder to scan for duplicate files");
        let mut info: BROWSEINFOW = std::mem::zeroed();
        info.hwndOwner = app().main;
        info.lpszTitle = title.as_ptr();
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        let selected = SHBrowseForFolderW(&info);
        if selected.is_null() {
            return;
        }
        let mut buffer = [0u16; 260];
        if SHGetPathFromIDListW(selected, buffer.as_mut_ptr()) != 0 {
            let chosen = from_wide(&buffer);
            set_text(app().path_label, &chosen);
            app().folder = Some(PathBuf::from(chosen));
            clear_surface();
            refresh_category_labels();
            set_text(
                app().status,
                "Folder selected. Choose Surface Scan to inventory user files.",
            );
        }
        update_controls();
    }

    unsafe fn begin_operation(status: &str) -> Arc<AtomicBool> {
        let state = app();
        let flag = Arc::new(AtomicBool::new(false));
        state.cancel_flag = Some(flag.clone());
        state.scanning = true;
        state.started_at = GetTickCount64();
        SetTimer(state.main, ID_ELAPSED_TIMER, 250, None);
        SendMessageW(state.progress, PBM_SETMARQUEE, 1, 30);
        set_text(state.status, status);
        update_controls();
        flag
    }

    unsafe fn end_operation() {
        let state = app();
        state.scanning = false;
        state.cancel_flag = None;
        KillTimer(state.main, ID_ELAPSED_TIMER);
        SendMessageW(state.progress, PBM_SETMARQUEE, 0, 0);
        SendMessageW(state.progress, PBM_SETPOS, 0, 0);
        update_controls();
    }

    /// Posting for every file would flood the message queue, so progress is
    /// throttled to roughly one message per 64 items.
    fn post_progress(main: isize, last: &mut usize, stage: Stage, done: usize, total: usize) {
        if done != 0 && done > *last && done - *last < 64 && done != total {
            return;
        }
        *last = done;
        let message = Box::new(ProgressMessage {
            stage: stage.label(),
            done,
            total,
        });
        unsafe {
            PostMessageW(
                main as HWND,
                WM_APP_PROGRESS,
                0,
                Box::into_raw(message) as isize,
            );
        }
    }

    /// Errors travel as small negative payloads so the UI thread can tell
    /// them apart from a boxed result pointer.
    fn error_code(error: &ScanError) -> isize {
        match error {
            ScanError::Cancelled => 1,
            ScanError::ProtectedRoot => 2,
            ScanError::LimitExceeded => 3,
        }
    }

    unsafe fn start_surface_scan() {
        let state = app();
        if state.scanning {
            return;
        }
        let Some(root) = state.folder.clone() else {
            MessageBoxW(
                state.main,
                w!("Choose a folder before scanning."),
                w!("Select Folder"),
                MB_OK | MB_ICONINFORMATION,
            );
            return;
        };
        clear_surface();
        refresh_category_labels();

        let flag = begin_operation("Surface scanning: inventorying user files...");
        let main = state.main as isize;
        std::thread::spawn(move || {
            let mut last = 0usize;
            let outcome = surface_scan(&root, &Options::default(), |stage, done, total| {
                if flag.load(Ordering::Relaxed) {
                    return Flow::Cancel;
                }
                post_progress(main, &mut last, stage, done, total);
                Flow::Continue
            });
            let payload = match outcome {
                Ok(report) => Box::into_raw(Box::new(report)) as isize,
                Err(error) => -error_code(&error),
            };
            unsafe {
                PostMessageW(main as HWND, WM_APP_SURFACE, 0, payload);
            }
        });
    }

    unsafe fn start_duplicate_scan() {
        let state = app();
        if state.scanning {
            return;
        }
        let Some(surface) = state.surface.clone() else {
            return;
        };
        let categories = selected_categories();
        if categories.is_empty() {
            MessageBoxW(
                state.main,
                w!("Select at least one file type before finding duplicates."),
                w!("Select File Types"),
                MB_OK | MB_ICONINFORMATION,
            );
            return;
        }
        clear_result();

        let options = Options {
            categories,
            ..Default::default()
        };
        let flag = begin_operation("Finding duplicates in the selected file types...");
        let main = state.main as isize;
        std::thread::spawn(move || {
            let mut last = 0usize;
            let outcome = find_duplicates(&surface, &options, |stage, done, total| {
                if flag.load(Ordering::Relaxed) {
                    return Flow::Cancel;
                }
                post_progress(main, &mut last, stage, done, total);
                Flow::Continue
            });
            let payload = match outcome {
                Ok(result) => Box::into_raw(Box::new(result)) as isize,
                Err(error) => -error_code(&error),
            };
            unsafe {
                PostMessageW(main as HWND, WM_APP_RESULT, 0, payload);
            }
        });
    }

    unsafe fn request_cancel() {
        let state = app();
        if !state.scanning {
            return;
        }
        if let Some(flag) = &state.cancel_flag {
            flag.store(true, Ordering::Relaxed);
        }
        set_text(state.status, "Cancelling...");
    }

    unsafe fn show_in_explorer() {
        let state = app();
        let index = SendMessageW(
            state.list,
            LVM_GETNEXTITEM,
            usize::MAX,
            LVNI_SELECTED as isize,
        );
        if index < 0 {
            return;
        }
        let Some(row) = state.rows.get(index as usize) else {
            return;
        };
        // /select, highlights the file in Explorer without opening it.
        let argument = wide(&format!("/select,\"{}\"", row.full_path.display()));
        ShellExecuteW(
            state.main,
            w!("open"),
            w!("explorer.exe"),
            argument.as_ptr(),
            std::ptr::null(),
            SW_SHOWNORMAL,
        );
    }

    /// Persist window placement and the last scanned folder. Failures are
    /// ignored: preferences are a convenience, never a precondition.
    unsafe fn save_settings() {
        let state = app();
        let Some(path) = state.settings_path.clone() else {
            return;
        };
        let mut value = Settings {
            last_folder: state.folder.clone(),
            ..Default::default()
        };
        let mut placement: WINDOWPLACEMENT = std::mem::zeroed();
        placement.length = std::mem::size_of::<WINDOWPLACEMENT>() as u32;
        if GetWindowPlacement(state.main, &mut placement) != 0 {
            value.x = placement.rcNormalPosition.left;
            value.y = placement.rcNormalPosition.top;
            value.width = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
            value.height = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
            value.maximized = placement.showCmd == SW_SHOWMAXIMIZED as u32;
        }
        let _ = settings::save(&path, &value);
    }

    /// Restore placement, clamping to the virtual screen so a window saved on
    /// a monitor that is no longer attached cannot open off-screen.
    unsafe fn apply_settings(value: &Settings) {
        let state = app();
        if value.has_placement() {
            let left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            let top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            let right = left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            let bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if value.x < right
                && value.y < bottom
                && value.x + value.width > left
                && value.y + value.height > top
            {
                MoveWindow(state.main, value.x, value.y, value.width, value.height, 1);
            }
        }
        if value.maximized {
            ShowWindow(state.main, SW_SHOWMAXIMIZED);
        }
        // Only offer the stored folder if it still exists.
        if let Some(folder) = &value.last_folder {
            if folder.is_dir() {
                set_text(state.path_label, &folder.to_string_lossy());
                state.folder = Some(folder.clone());
                set_text(
                    state.status,
                    "Restored your last folder. Choose Surface Scan to inventory user files.",
                );
            }
        }
    }

    /// Discard duplicate results but keep the surface inventory, so the user
    /// can adjust the file-type focus and search again without rescanning.
    unsafe fn clear_results_action() {
        clear_result();
        let state = app();
        set_text(state.stage_label, "");
        set_text(
            state.status,
            "Results cleared. The surface inventory is still available; \
             adjust the file-type focus and choose Find Duplicates.",
        );
        update_controls();
    }

    /// Return to the initial state, discarding folder, inventory, and results.
    unsafe fn reset_action() {
        clear_surface();
        let state = app();
        state.folder = None;
        set_text(state.path_label, "No folder selected.");
        set_text(state.stage_label, "");
        set_text(state.elapsed_label, "");
        set_text(state.files_label, "");
        set_text(state.current_label, "");
        refresh_category_labels();
        set_text(state.status, "Reset. Select a folder to begin.");
        update_controls();
    }

    unsafe fn show_about() {
        MessageBoxW(
            app().main,
            w!("TwinTidy (Rust port)\r\n\r\n\
                A Rust port of the duplicate-detection core of TwinTidy, the \
                safety-first Windows duplicate-file finder by Kayilan Inc.\r\n\r\n\
                This window is read-only: it finds and reports duplicates and \
                never deletes, moves, or modifies a file. Checkbox selection \
                plans cleanup for export; it does not perform it.\r\n\r\n\
                Protected system folders, dependency trees, build output, and \
                executable file types are excluded from every scan.\r\n\r\n\
                MIT licensed. Copyright (c) 2026 Kayilan Inc."),
            w!("About TwinTidy"),
            MB_OK | MB_ICONINFORMATION,
        );
    }

    unsafe fn show_preview_safety() {
        MessageBoxW(
            app().main,
            w!(
                "This port does not open, render, or preview the contents of \
                scanned files.\r\n\r\n\
                Files are read only to compute their size and hash, and to \
                compare candidates byte for byte. Nothing scanned is parsed, \
                executed, or loaded into a viewer.\r\n\r\n\
                Use Show In Explorer, then open a file with an application you \
                trust if you need to inspect its contents."
            ),
            w!("Preview Safety"),
            MB_OK | MB_ICONINFORMATION,
        );
    }

    unsafe fn export_report() {
        let state = app();
        let Some(result) = &state.result else {
            return;
        };
        if result.groups.is_empty() {
            return;
        }

        let mut path = wide("twintidy-duplicates.csv");
        path.resize(260, 0);
        let filter: Vec<u16> = "CSV report\0*.csv\0JSON report\0*.json\0\0"
            .encode_utf16()
            .collect();
        let default_extension = wide("csv");

        let mut dialog: OPENFILENAMEW = std::mem::zeroed();
        dialog.lStructSize = std::mem::size_of::<OPENFILENAMEW>() as u32;
        dialog.hwndOwner = state.main;
        dialog.lpstrFilter = filter.as_ptr();
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = path.as_mut_ptr();
        dialog.nMaxFile = path.len() as u32;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        dialog.lpstrDefExt = default_extension.as_ptr();

        if GetSaveFileNameW(&mut dialog) == 0 {
            return;
        }
        let format = if dialog.nFilterIndex == 2 {
            Format::Json
        } else {
            Format::Csv
        };
        let destination = PathBuf::from(from_wide(&path));
        let folder = state
            .folder
            .as_ref()
            .map(|value| value.to_string_lossy().into_owned())
            .unwrap_or_default();

        match report::write_file(&destination, format, &folder, result, now_unix()) {
            Ok(()) => {
                let message = format!("Report saved to {}", destination.display());
                set_text(state.status, &message);
            }
            Err(_) => {
                MessageBoxW(
                    state.main,
                    w!("The report could not be written. No existing file was replaced."),
                    w!("Export Failed"),
                    MB_OK | MB_ICONWARNING,
                );
            }
        }
    }

    // ---------- message handlers ----------

    unsafe fn on_progress(raw: isize) {
        if raw <= 0 {
            return;
        }
        let message = Box::from_raw(raw as *mut ProgressMessage);
        let state = app();
        set_text(state.stage_label, message.stage);
        let counts = if message.total > 0 {
            format!("{} / {} files", message.done, message.total)
        } else {
            format!("{} files", message.done)
        };
        set_text(state.files_label, &counts);
        if message.total > 0 {
            SendMessageW(state.progress, PBM_SETMARQUEE, 0, 0);
            SetWindowLongPtrW(
                state.progress,
                GWL_STYLE,
                GetWindowLongPtrW(state.progress, GWL_STYLE) & !(PBS_MARQUEE as isize),
            );
            SendMessageW(state.progress, PBM_SETRANGE32, 0, message.total as isize);
            SendMessageW(state.progress, PBM_SETPOS, message.done, 0);
        }
    }

    unsafe fn on_surface_finished(raw: isize) {
        end_operation();
        let state = app();
        if raw <= 0 {
            let text = match -raw {
                1 => "Surface scan cancelled. No files were changed.",
                2 => "That folder is a protected system or build location and cannot be scanned.",
                3 => "The folder contains too many items to scan. Select a smaller folder.",
                _ => "Surface scan failed. No files were changed.",
            };
            set_text(state.status, text);
            return;
        }

        let surface = *Box::from_raw(raw as *mut SurfaceReport);
        let summary = format!(
            "{} user file(s), {} across {} folder(s). {} protected item(s) skipped. \
             Choose file types, then Find Duplicates.",
            surface.files.len(),
            format_bytes(surface.total_bytes),
            surface.directories_scanned,
            surface.skipped_system_items
        );
        state.surface = Some(surface);
        refresh_category_labels();
        set_text(state.status, &summary);
        update_controls();
    }

    unsafe fn on_result_finished(raw: isize) {
        end_operation();
        let state = app();
        if raw <= 0 {
            let text = match -raw {
                1 => "Duplicate scan cancelled. The surface inventory remains available.",
                _ => "Duplicate scan failed. No files were changed.",
            };
            set_text(state.status, text);
            return;
        }

        let result = *Box::from_raw(raw as *mut ScanResult);
        state.rows = build_rows(&result);
        SendMessageW(state.list, LVM_SETITEMCOUNT, state.rows.len(), 0);
        InvalidateRect(state.list, std::ptr::null(), 1);

        let summary = if result.groups.is_empty() {
            format!(
                "No duplicates found among {} file(s) in the selected types. \
                 Change the file-type focus and choose Find Duplicates again.",
                result.files_considered
            )
        } else {
            format!(
                "{} duplicate group(s) across {} file(s). Keeping one copy of each would reclaim {}.",
                result.groups.len(),
                state.rows.len(),
                format_bytes(result.reclaimable)
            )
        };
        state.result = Some(result);
        set_text(state.status, &summary);
        update_controls();
    }

    /// Virtual list view: fill only the cells Windows asks for, and report
    /// checkbox state from our own model since owner-data views store none.
    unsafe fn on_get_display_info(info: *mut NMLVDISPINFOW) {
        let item = &mut (*info).item;
        let state = app();
        let Some(row) = state.rows.get(item.iItem as usize) else {
            return;
        };

        if item.mask & LVIF_STATE != 0 {
            item.stateMask = LVIS_STATEIMAGEMASK;
            // State image 1 is unchecked, 2 is checked (INDEXTOSTATEIMAGEMASK).
            item.state = if row.checked { 2 << 12 } else { 1 << 12 };
        }
        if item.mask & LVIF_TEXT == 0 {
            return;
        }
        // The scratch buffer must outlive this callback; the row owns the
        // borrowed strings, and formatted columns reuse this buffer.
        static mut SCRATCH: Vec<u16> = Vec::new();
        match item.iSubItem {
            // Columns whose text is formatted on demand.
            0 | 1 | 3 | 5 => {
                let text = match item.iSubItem {
                    // No. — 1-based row number, matching the exported order.
                    0 => (item.iItem + 1).to_string(),
                    1 => row.group.to_string(),
                    3 => format_bytes(row.size),
                    _ => format_timestamp(row.modified_at),
                };
                #[allow(static_mut_refs)]
                {
                    SCRATCH = wide(&text);
                    item.pszText = SCRATCH.as_mut_ptr();
                }
            }
            // Columns backed by strings the row already owns.
            2 => item.pszText = row.name.as_ptr() as *mut u16,
            4 => item.pszText = row.kind.as_ptr() as *mut u16,
            6 => item.pszText = row.hash.as_ptr() as *mut u16,
            7 => item.pszText = row.folder.as_ptr() as *mut u16,
            _ => {}
        }
    }

    /// Owner-data views do not toggle checkboxes themselves; hit-test the
    /// click against the state icon and update our model.
    unsafe fn on_list_click() {
        let state = app();
        let mut hit: LVHITTESTINFO = std::mem::zeroed();
        let mut point = POINT { x: 0, y: 0 };
        GetCursorPos(&mut point);
        ScreenToClient(state.list, &mut point);
        hit.pt = point;
        let index = SendMessageW(state.list, LVM_HITTEST, 0, &mut hit as *mut _ as isize);
        if index >= 0 && (hit.flags & LVHT_ONITEMSTATEICON) != 0 {
            if let Some(row) = state.rows.get_mut(index as usize) {
                row.checked = !row.checked;
                SendMessageW(state.list, LVM_REDRAWITEMS, index as usize, index);
            }
        }
        update_controls();
    }

    // ---------- layout ----------

    unsafe fn layout(width: i32, height: i32) {
        let state = app();
        // All metrics are 96-DPI design values passed through scaled(), so
        // the window keeps its proportions on any display scaling.
        let margin = scaled(12);
        let line = scaled(32);
        let button = scaled(140);

        let mut y = margin;
        MoveWindow(state.select_button, margin, y, button, line, 1);
        MoveWindow(
            state.path_label,
            margin * 2 + button,
            y + scaled(6),
            width - (margin * 3 + button),
            line,
            1,
        );

        y += line + scaled(8);
        MoveWindow(
            state.focus_label,
            margin,
            y,
            width - margin * 2,
            scaled(20),
            1,
        );
        y += scaled(24);

        // Category checkboxes wrap across the available width.
        let check_width = scaled(150);
        let mut column = 0;
        for check in &state.category_checks {
            let mut x = margin + column * check_width;
            if x + check_width > width - margin && column > 0 {
                column = 0;
                x = margin;
                y += scaled(26);
            }
            MoveWindow(*check, x, y, check_width - scaled(6), scaled(24), 1);
            column += 1;
        }

        y += scaled(36);
        MoveWindow(state.surface_button, margin, y, button, line, 1);
        MoveWindow(state.find_button, margin * 2 + button, y, button, line, 1);
        MoveWindow(
            state.cancel_button,
            margin * 3 + button * 2,
            y,
            button,
            line,
            1,
        );
        let progress_width = (width - (margin * 5 + button * 3 + scaled(190))).max(scaled(60));
        MoveWindow(
            state.progress,
            margin * 4 + button * 3,
            y + scaled(6),
            progress_width,
            line - scaled(12),
            1,
        );
        MoveWindow(
            state.stage_label,
            width - margin - scaled(186),
            y + scaled(8),
            scaled(120),
            scaled(20),
            1,
        );
        MoveWindow(
            state.elapsed_label,
            width - margin - scaled(66),
            y + scaled(8),
            scaled(66),
            scaled(20),
            1,
        );

        y += line + margin;
        MoveWindow(state.keep_newest_button, margin, y, button, line, 1);
        MoveWindow(
            state.keep_oldest_button,
            margin * 2 + button,
            y,
            button,
            line,
            1,
        );
        MoveWindow(
            state.clear_select_button,
            margin * 3 + button * 2,
            y,
            button,
            line,
            1,
        );
        MoveWindow(
            state.export_button,
            margin * 4 + button * 3,
            y,
            button,
            line,
            1,
        );
        MoveWindow(
            state.explorer_button,
            margin * 5 + button * 4,
            y,
            button + scaled(20),
            line,
            1,
        );

        y += line + scaled(8);
        MoveWindow(state.clear_results_button, margin, y, button, line, 1);
        MoveWindow(state.reset_button, margin * 2 + button, y, button, line, 1);
        MoveWindow(
            state.preview_safety_button,
            margin * 3 + button * 2,
            y,
            button,
            line,
            1,
        );
        MoveWindow(
            state.about_button,
            margin * 4 + button * 3,
            y,
            button,
            line,
            1,
        );
        MoveWindow(
            state.files_label,
            margin * 5 + button * 4,
            y + scaled(8),
            scaled(140),
            scaled(20),
            1,
        );
        MoveWindow(
            state.current_label,
            margin * 5 + button * 4 + scaled(150),
            y + scaled(8),
            width - (margin * 6 + button * 4 + scaled(150)),
            scaled(20),
            1,
        );

        y += line + margin;
        let status_height = scaled(38);
        let list_height = (height - y - status_height - margin).max(scaled(60));
        MoveWindow(state.list, margin, y, width - margin * 2, list_height, 1);
        MoveWindow(
            state.status,
            margin,
            y + list_height + scaled(6),
            width - margin * 2,
            status_height - scaled(6),
            1,
        );
    }

    unsafe extern "system" fn window_procedure(
        window: HWND,
        message: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> LRESULT {
        match message {
            WM_CREATE => {
                app().dpi = display_dpi(window);
                create_controls(window);
                if let Some(path) = settings::default_path() {
                    let stored = settings::load(&path);
                    app().settings_path = Some(path);
                    apply_settings(&stored);
                }
                update_controls();
                0
            }
            WM_SIZE => {
                layout((lparam & 0xFFFF) as i32, ((lparam >> 16) & 0xFFFF) as i32);
                0
            }
            WM_GETMINMAXINFO => {
                let limits = lparam as *mut MINMAXINFO;
                (*limits).ptMinTrackSize.x = scaled(980);
                (*limits).ptMinTrackSize.y = scaled(600);
                0
            }
            WM_TIMER => {
                if wparam == ID_ELAPSED_TIMER {
                    let elapsed = (GetTickCount64() - app().started_at) / 1000;
                    let text = format!("{}:{:02}", elapsed / 60, elapsed % 60);
                    set_text(app().elapsed_label, &text);
                }
                0
            }
            WM_COMMAND => {
                match wparam & 0xFFFF {
                    ID_SELECT => choose_folder(),
                    ID_SURFACE => start_surface_scan(),
                    ID_FIND => start_duplicate_scan(),
                    ID_CANCEL => request_cancel(),
                    ID_KEEP_NEWEST => select_all_except(true),
                    ID_KEEP_OLDEST => select_all_except(false),
                    ID_CLEAR_SELECT => clear_selection(),
                    ID_EXPORT => export_report(),
                    ID_EXPLORER => show_in_explorer(),
                    ID_CLEAR_RESULTS => clear_results_action(),
                    ID_RESET => reset_action(),
                    ID_ABOUT => show_about(),
                    ID_PREVIEW_SAFETY => show_preview_safety(),
                    _ => return DefWindowProcW(window, message, wparam, lparam),
                }
                0
            }
            WM_NOTIFY => {
                let header = lparam as *const NMHDR;
                if (*header).idFrom != ID_LIST {
                    return DefWindowProcW(window, message, wparam, lparam);
                }
                match (*header).code {
                    LVN_GETDISPINFOW => {
                        on_get_display_info(lparam as *mut NMLVDISPINFOW);
                        0
                    }
                    NM_CLICK => {
                        on_list_click();
                        0
                    }
                    LVN_ITEMCHANGED => {
                        update_controls();
                        0
                    }
                    _ => DefWindowProcW(window, message, wparam, lparam),
                }
            }
            WM_APP_PROGRESS => {
                on_progress(lparam);
                0
            }
            WM_APP_SURFACE => {
                on_surface_finished(lparam);
                0
            }
            WM_APP_RESULT => {
                on_result_finished(lparam);
                0
            }
            WM_CLOSE => {
                // A scan owns a worker thread; ask it to stop and let the
                // completion message arrive before tearing the window down.
                if app().scanning {
                    request_cancel();
                    return 0;
                }
                DestroyWindow(window);
                0
            }
            WM_DESTROY => {
                save_settings();
                let state = app();
                if !state.font.is_null() {
                    DeleteObject(state.font);
                }
                PostQuitMessage(0);
                0
            }
            _ => DefWindowProcW(window, message, wparam, lparam),
        }
    }
}
