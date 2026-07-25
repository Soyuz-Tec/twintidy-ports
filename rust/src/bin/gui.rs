//! Native Win32 front end for the TwinTidy Rust port.
//!
//! Read-only by construction: this window can select a folder, scan it, and
//! review the results. It exposes no action that deletes or modifies a file.
//!
//! The scan runs on a worker thread and reports progress by posting messages
//! to the UI thread, so the window stays responsive and cancellable.
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

    use twintidy::{format_bytes, scan, Flow, Options, ScanResult, Stage};

    use windows_sys::core::{w, PCWSTR};
    use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
    use windows_sys::Win32::Graphics::Gdi::{
        CreateFontW, DeleteObject, InvalidateRect, UpdateWindow, CLEARTYPE_QUALITY,
        CLIP_DEFAULT_PRECIS, COLOR_BTNFACE, DEFAULT_CHARSET, DEFAULT_PITCH, FF_DONTCARE, FW_NORMAL,
        HFONT, OUT_DEFAULT_PRECIS,
    };
    use windows_sys::Win32::System::Com::{
        CoInitializeEx, CoUninitialize, COINIT_APARTMENTTHREADED,
    };
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::System::SystemServices::SS_PATHELLIPSIS;
    use windows_sys::Win32::UI::Controls::{
        InitCommonControlsEx, INITCOMMONCONTROLSEX, LVCFMT_LEFT, LVCF_SUBITEM, LVCF_TEXT,
        LVCF_WIDTH, LVCOLUMNW, LVIF_TEXT, LVM_INSERTCOLUMNW, LVM_SETEXTENDEDLISTVIEWSTYLE,
        LVM_SETITEMCOUNT, LVN_GETDISPINFOW, LVS_EX_FULLROWSELECT, LVS_EX_GRIDLINES, LVS_OWNERDATA,
        LVS_REPORT, LVS_SHOWSELALWAYS, NMHDR, NMLVDISPINFOW, PBM_SETMARQUEE, PBM_SETPOS,
        PBM_SETRANGE32, PBS_MARQUEE, WC_LISTVIEWW,
    };
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::EnableWindow;
    use windows_sys::Win32::UI::Shell::{
        SHBrowseForFolderW, SHGetPathFromIDListW, BIF_NEWDIALOGSTYLE, BIF_RETURNONLYFSDIRS,
        BROWSEINFOW,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::*;

    const ID_SELECT: usize = 1001;
    const ID_SCAN: usize = 1002;
    const ID_CANCEL: usize = 1003;
    const ID_LIST: usize = 1004;

    const WM_APP_PROGRESS: u32 = WM_APP + 1;
    const WM_APP_FINISHED: u32 = WM_APP + 2;

    /// One display row: a single file inside a duplicate group.
    struct Row {
        group: usize,
        size: u64,
        name: Vec<u16>,
        folder: Vec<u16>,
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
        scan_button: HWND,
        cancel_button: HWND,
        list: HWND,
        progress: HWND,
        status: HWND,
        path_label: HWND,
        font: HFONT,
        folder: Option<PathBuf>,
        rows: Vec<Row>,
        scanning: bool,
        cancel_flag: Option<Arc<AtomicBool>>,
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

            let window = CreateWindowExW(
                0,
                class_name,
                w!("TwinTidy (Rust port) - Duplicate File Review"),
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                1000,
                620,
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

    unsafe fn create_controls(window: HWND) {
        let state = app();
        state.main = window;
        state.font = CreateFontW(
            -15,
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
        let child = |class: PCWSTR, text: PCWSTR, style: u32, id: usize, ex: u32| -> HWND {
            CreateWindowExW(
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
            )
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
        state.scan_button = child(
            w!("BUTTON"),
            w!("Scan"),
            BS_DEFPUSHBUTTON as u32,
            ID_SCAN,
            0,
        );
        state.cancel_button = child(
            w!("BUTTON"),
            w!("Cancel"),
            BS_PUSHBUTTON as u32,
            ID_CANCEL,
            0,
        );
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
            w!("Select a folder, then choose Scan. This window never changes your files."),
            SS_PATHELLIPSIS,
            0,
            0,
        );

        SendMessageW(
            state.list,
            LVM_SETEXTENDEDLISTVIEWSTYLE,
            0,
            (LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES) as isize,
        );

        for (index, (title, width)) in
            [("Group", 70), ("Size", 100), ("File", 280), ("Folder", 520)]
                .iter()
                .enumerate()
        {
            let mut heading = wide(title);
            let column = LVCOLUMNW {
                mask: LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM,
                fmt: LVCFMT_LEFT,
                cx: *width,
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

        for control in [
            state.select_button,
            state.path_label,
            state.scan_button,
            state.cancel_button,
            state.list,
            state.status,
        ] {
            SendMessageW(control, WM_SETFONT, state.font as usize, 1);
        }
    }

    unsafe fn layout(width: i32, height: i32) {
        let state = app();
        let margin = 12;
        let row_height = 28;
        let button_width = 130;

        MoveWindow(
            state.select_button,
            margin,
            margin,
            button_width,
            row_height,
            1,
        );
        MoveWindow(
            state.path_label,
            margin * 2 + button_width,
            margin + 4,
            width - (margin * 3 + button_width),
            row_height,
            1,
        );

        let second = margin * 2 + row_height;
        MoveWindow(
            state.scan_button,
            margin,
            second,
            button_width,
            row_height,
            1,
        );
        MoveWindow(
            state.cancel_button,
            margin * 2 + button_width,
            second,
            button_width,
            row_height,
            1,
        );
        MoveWindow(
            state.progress,
            margin * 3 + button_width * 2,
            second + 4,
            width - (margin * 4 + button_width * 2),
            row_height - 8,
            1,
        );

        let third = second + row_height + margin;
        let status_height = 20;
        let list_height = (height - third - status_height - margin * 2).max(40);
        MoveWindow(
            state.list,
            margin,
            third,
            width - margin * 2,
            list_height,
            1,
        );
        MoveWindow(
            state.status,
            margin,
            third + list_height + 8,
            width - margin * 2,
            status_height,
            1,
        );
    }

    unsafe fn update_controls() {
        let state = app();
        let busy = state.scanning;
        EnableWindow(state.select_button, (!busy) as i32);
        EnableWindow(state.scan_button, (!busy && state.folder.is_some()) as i32);
        EnableWindow(state.cancel_button, busy as i32);
    }

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
            set_text(
                app().status,
                "Folder selected. Choose Scan to find duplicate files.",
            );
        }
        update_controls();
    }

    unsafe fn start_scan() {
        let state = app();
        if state.scanning {
            return;
        }
        let Some(root) = state.folder.clone() else {
            return;
        };

        state.rows.clear();
        SendMessageW(state.list, LVM_SETITEMCOUNT, 0, 0);
        InvalidateRect(state.list, std::ptr::null(), 1);

        let cancel_flag = Arc::new(AtomicBool::new(false));
        state.cancel_flag = Some(cancel_flag.clone());
        state.scanning = true;
        SendMessageW(state.progress, PBM_SETMARQUEE, 1, 30);
        set_text(state.status, "Scanning...");
        update_controls();

        let main = state.main as isize;
        std::thread::spawn(move || {
            let mut last_posted = 0usize;
            let options = Options::default();
            let outcome = scan(
                &root,
                &options,
                |stage: Stage, done: usize, total: usize| {
                    if cancel_flag.load(Ordering::Relaxed) {
                        return Flow::Cancel;
                    }
                    // Throttle: posting for every file would flood the queue.
                    if done != 0 && done - last_posted < 64 && done != total {
                        return Flow::Continue;
                    }
                    last_posted = done;
                    let message = Box::new(ProgressMessage {
                        stage: stage.label(),
                        done,
                        total,
                    });
                    unsafe {
                        if PostMessageW(
                            main as HWND,
                            WM_APP_PROGRESS,
                            0,
                            Box::into_raw(message) as isize,
                        ) == 0
                        {
                            // Window is gone; nothing to leak into.
                        }
                    }
                    Flow::Continue
                },
            );

            let payload: isize = match outcome {
                Ok(result) => Box::into_raw(Box::new(result)) as isize,
                Err(_) => 0,
            };
            unsafe {
                PostMessageW(main as HWND, WM_APP_FINISHED, 0, payload);
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

    unsafe fn on_progress(raw: isize) {
        if raw == 0 {
            return;
        }
        let message = Box::from_raw(raw as *mut ProgressMessage);
        let state = app();
        if message.total > 0 {
            SendMessageW(state.progress, PBM_SETMARQUEE, 0, 0);
            SetWindowLongPtrW(
                state.progress,
                GWL_STYLE,
                GetWindowLongPtrW(state.progress, GWL_STYLE) & !(PBS_MARQUEE as isize),
            );
            SendMessageW(state.progress, PBM_SETRANGE32, 0, message.total as isize);
            SendMessageW(state.progress, PBM_SETPOS, message.done, 0);
            set_text(
                state.status,
                &format!("{}  ({} of {})", message.stage, message.done, message.total),
            );
        } else {
            set_text(
                state.status,
                &format!("{}  ({} file(s) found)", message.stage, message.done),
            );
        }
    }

    /// Flatten confirmed groups into display rows, splitting each path into a
    /// file name and its containing folder so both columns stay readable.
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
                rows.push(Row {
                    group: index + 1,
                    size: file.size,
                    name: wide(&name),
                    folder: wide(&folder),
                });
            }
        }
        rows
    }

    unsafe fn on_finished(raw: isize) {
        let state = app();
        state.scanning = false;
        state.cancel_flag = None;
        SendMessageW(state.progress, PBM_SETMARQUEE, 0, 0);
        SendMessageW(state.progress, PBM_SETPOS, 0, 0);

        if raw == 0 {
            set_text(state.status, "Scan cancelled. No files were changed.");
            update_controls();
            return;
        }

        let result = *Box::from_raw(raw as *mut ScanResult);
        state.rows = build_rows(&result);
        SendMessageW(state.list, LVM_SETITEMCOUNT, state.rows.len(), 0);
        InvalidateRect(state.list, std::ptr::null(), 1);

        let summary = if result.groups.is_empty() {
            format!(
                "No duplicates found among {} scanned file(s).",
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
        set_text(state.status, &summary);
        update_controls();
    }

    /// Virtual list view: fill only the cells Windows actually asks for.
    unsafe fn on_get_display_info(info: *mut NMLVDISPINFOW) {
        let item = &mut (*info).item;
        if item.mask & LVIF_TEXT == 0 {
            return;
        }
        let state = app();
        let index = item.iItem as usize;
        let Some(row) = state.rows.get(index) else {
            return;
        };
        // The buffers must outlive this callback; the row owns the strings and
        // the scratch buffer is reused for the two formatted columns.
        static mut SCRATCH: Vec<u16> = Vec::new();
        match item.iSubItem {
            0 => {
                #[allow(static_mut_refs)]
                {
                    SCRATCH = wide(&row.group.to_string());
                    item.pszText = SCRATCH.as_mut_ptr();
                }
            }
            1 => {
                #[allow(static_mut_refs)]
                {
                    SCRATCH = wide(&format_bytes(row.size));
                    item.pszText = SCRATCH.as_mut_ptr();
                }
            }
            2 => item.pszText = row.name.as_ptr() as *mut u16,
            3 => item.pszText = row.folder.as_ptr() as *mut u16,
            _ => {}
        }
    }

    unsafe extern "system" fn window_procedure(
        window: HWND,
        message: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> LRESULT {
        match message {
            WM_CREATE => {
                create_controls(window);
                update_controls();
                0
            }
            WM_SIZE => {
                layout((lparam & 0xFFFF) as i32, ((lparam >> 16) & 0xFFFF) as i32);
                0
            }
            WM_GETMINMAXINFO => {
                let limits = lparam as *mut MINMAXINFO;
                (*limits).ptMinTrackSize.x = 720;
                (*limits).ptMinTrackSize.y = 400;
                0
            }
            WM_COMMAND => {
                match wparam & 0xFFFF {
                    ID_SELECT => choose_folder(),
                    ID_SCAN => start_scan(),
                    ID_CANCEL => request_cancel(),
                    _ => return DefWindowProcW(window, message, wparam, lparam),
                }
                0
            }
            WM_NOTIFY => {
                let header = lparam as *const NMHDR;
                if (*header).idFrom == ID_LIST && (*header).code == LVN_GETDISPINFOW {
                    on_get_display_info(lparam as *mut NMLVDISPINFOW);
                    return 0;
                }
                DefWindowProcW(window, message, wparam, lparam)
            }
            WM_APP_PROGRESS => {
                on_progress(lparam);
                0
            }
            WM_APP_FINISHED => {
                on_finished(lparam);
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
