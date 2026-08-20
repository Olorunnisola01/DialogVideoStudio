// Portable single-file launcher for Dialog Video Studio.
//
// Layout of the shipped file:   [ this launcher .exe ][ appended 7z archive ]
//
// First run unpacks the archive into "EdgeTTSStudio\" beside the launcher and
// starts the app. Every later run sees the marker file and starts the app
// immediately, so the unpack cost is paid exactly once.
//
// 7z.exe / 7z.dll are embedded as resources and used to read the archive that
// is appended to this very file (7-Zip locates the archive signature itself,
// which is how ordinary SFX archives work).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define IDR_7Z_EXE 101
#define IDR_7Z_DLL 102

static const wchar_t* kAppName    = L"Dialog Video Studio";
static const wchar_t* kSubDir     = L"DialogVideoStudio";
static const wchar_t* kAppExeName = L"DialogVideoStudio.exe";
static const wchar_t* kMarker     = L".unpacked";

// Payload needs ~315 MB unpacked; ask for a little headroom.
static const ULONGLONG kRequiredBytes = 600ULL * 1024 * 1024;

static void Fail(const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), kAppName, MB_ICONERROR | MB_OK);
}

static bool PathExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring SelfPath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

static std::wstring ParentDir(const std::wstring& p) {
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

// Write an embedded RCDATA resource out to disk.
static bool WriteResourceToFile(int resId, const std::wstring& outPath) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!res) return false;
    HGLOBAL h = LoadResource(nullptr, res);
    if (!h) return false;
    const void* data = LockResource(h);
    DWORD size = SizeofResource(nullptr, res);
    if (!data || !size) return false;

    HANDLE f = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

// ---- "unpacking" splash -----------------------------------------------------
// A plain window with a marquee bar, so a 1.7 GB first run does not look frozen.

static HWND g_splash = nullptr;

static LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowSplash(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc   = SplashProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DvsPortableSplash";
    RegisterClassW(&wc);

    int w = 460, h = 150;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_splash = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
                               kAppName, WS_POPUPWINDOW | WS_CAPTION,
                               x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_splash) return;

    CreateWindowExW(0, L"STATIC",
        L"Setting up on first run \x2014 unpacking the voice models.\n"
        L"This happens once; later starts are instant.",
        WS_CHILD | WS_VISIBLE, 20, 18, 420, 42, g_splash, nullptr, inst, nullptr);

    HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 20, 72, 420, 20,
        g_splash, nullptr, inst, nullptr);
    SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);

    ShowWindow(g_splash, SW_SHOW);
    UpdateWindow(g_splash);
}

static void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---- extraction -------------------------------------------------------------

// Runs 7z.exe hidden, keeping the splash responsive. Returns the exit code,
// or -1 if the process could not be started.
static int RunHiddenAndWait(const std::wstring& cmdLine) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::wstring mutableCmd = cmdLine;  // CreateProcessW may modify the buffer
    if (!CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    CloseHandle(pi.hThread);

    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        PumpMessages();
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// Starts the unpacked app and returns immediately.
//
// CreateProcessW rather than ShellExecuteEx: ShellExecuteEx needs COM to be
// initialised on the calling thread, and without that it can report success
// while silently starting nothing.
static bool LaunchApp(const std::wstring& exePath, const std::wstring& workDir) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring cmd = L"\"" + exePath + L"\"";
    if (!CreateProcessW(exePath.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                        0, nullptr, workDir.c_str(), &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool HasFreeSpace(const std::wstring& dir, ULONGLONG needed) {
    ULARGE_INTEGER freeForCaller{};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &freeForCaller, nullptr, nullptr)) {
        return true;  // cannot tell; let the extraction speak for itself
    }
    return freeForCaller.QuadPart >= needed;
}

static void RemoveDirRecursive(const std::wstring& dir) {
    std::wstring from = dir;
    from.push_back(L'\0');  // SHFileOperation wants a double-NUL terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc  = FO_DELETE;
    op.pFrom  = from.c_str();
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    const std::wstring self      = SelfPath();
    const std::wstring baseDir   = ParentDir(self);
    const std::wstring targetDir = baseDir + L"\\" + kSubDir;
    const std::wstring appExe    = targetDir + L"\\" + kAppExeName;
    const std::wstring marker    = targetDir + L"\\" + kMarker;

    // Fast path: already unpacked.
    if (PathExists(marker) && PathExists(appExe)) {
        if (!LaunchApp(appExe, targetDir)) {
            Fail(L"Could not start the application:\n" + appExe);
            return 1;
        }
        return 0;
    }

    if (!HasFreeSpace(baseDir, kRequiredBytes)) {
        Fail(L"Not enough free disk space.\n\n"
             L"About 2 GB is needed to unpack the voice models next to:\n" + self);
        return 1;
    }

    // A partial unpack from an interrupted earlier run must not be reused.
    if (PathExists(targetDir) && !PathExists(marker)) {
        RemoveDirRecursive(targetDir);
    }

    ShowSplash(inst);
    PumpMessages();

    // Drop the bundled 7-Zip next to nothing important, in our own temp dir.
    wchar_t tempRoot[MAX_PATH];
    GetTempPathW(ARRAYSIZE(tempRoot), tempRoot);
    std::wstring tempDir = std::wstring(tempRoot) + L"dvsx_" +
                           std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(tempDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        Fail(L"Could not create a temporary folder:\n" + tempDir);
        return 1;
    }

    const std::wstring sevenZipExe = tempDir + L"\\7z.exe";
    const std::wstring sevenZipDll = tempDir + L"\\7z.dll";
    if (!WriteResourceToFile(IDR_7Z_EXE, sevenZipExe) ||
        !WriteResourceToFile(IDR_7Z_DLL, sevenZipDll)) {
        RemoveDirRecursive(tempDir);
        Fail(L"Could not unpack the bundled extractor.");
        return 1;
    }

    // 7-Zip reads the archive appended to this launcher, exactly as it reads
    // any other SFX archive.
    std::wstring cmd = L"\"" + sevenZipExe + L"\" x \"" + self +
                       L"\" -o\"" + targetDir + L"\" -y -bso0 -bse0 -bsp0";
    int rc = RunHiddenAndWait(cmd);

    RemoveDirRecursive(tempDir);

    if (rc != 0 || !PathExists(appExe)) {
        if (PathExists(targetDir)) RemoveDirRecursive(targetDir);
        if (g_splash) DestroyWindow(g_splash);
        Fail(L"Unpacking failed (code " + std::to_wstring(rc) + L").\n\n"
             L"Try running the file from a folder you can write to, such as your Desktop.");
        return 1;
    }

    // Marker is written only after a verified-complete extraction.
    HANDLE m = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (m != INVALID_HANDLE_VALUE) CloseHandle(m);

    if (g_splash) DestroyWindow(g_splash);

    if (!LaunchApp(appExe, targetDir)) {
        Fail(L"Unpacked successfully, but could not start:\n" + appExe);
        return 1;
    }
    return 0;
}
