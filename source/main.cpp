#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <string>
#include "flash_loader.h"
#include "browser.h"
#include "resource.h"

static constexpr UINT BASE_DPI           = 96;
static constexpr int  TOOLBAR_HEIGHT_DIP = 32;
static constexpr int  BUTTON_WIDTH_DIP   = 60;
static constexpr int  BUTTON_HEIGHT_DIP  = 24;
static constexpr int  MARGIN_DIP         = 4;
static constexpr int  WINDOW_MIN_CX_DIP  = 500;
static constexpr int  WINDOW_MIN_CY_DIP  = 350;

enum ControlID {
    IDC_BACK = 1001,
    IDC_FORWARD,
    IDC_REFRESH,
    IDC_STOP,
    IDC_ADDRESS,
    IDC_GO,
    IDC_STATUS,
};

static bool         g_flashActivated  = false;
static BrowserHost* g_pBrowser        = nullptr;
static HWND         g_hwndMain        = nullptr;
static HWND         g_hwndBack        = nullptr;
static HWND         g_hwndForward     = nullptr;
static HWND         g_hwndRefresh     = nullptr;
static HWND         g_hwndStop        = nullptr;
static HWND         g_hwndAddress     = nullptr;
static HWND         g_hwndGo          = nullptr;
static HWND         g_hwndStatus      = nullptr;
static HFONT        g_controlFont     = nullptr;
static bool         g_isClosing       = false;
static UINT         g_dpi             = BASE_DPI;
static std::wstring g_initialAddress  = L"https://www.bing.com/";

static UINT GetWindowDpi(HWND hwnd)
{
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));

    if (getDpiForWindow) {
        UINT dpi = getDpiForWindow(hwnd);
        if (dpi != 0)
            return dpi;
    }

    // GetDpiForWindow is unavailable before Windows 10. GetDpiForMonitor
    // preserves per-monitor behavior on Windows 8.1 and is absent on Win7/8.
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto getDpiForMonitor = []() -> GetDpiForMonitorFn {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        return shcore ? reinterpret_cast<GetDpiForMonitorFn>(
                            GetProcAddress(shcore, "GetDpiForMonitor"))
                      : nullptr;
    }();

    if (getDpiForMonitor) {
        UINT dpiX = 0;
        UINT dpiY = 0;
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (SUCCEEDED(getDpiForMonitor(monitor, 0, &dpiX, &dpiY)) && dpiX != 0)
            return dpiX;
    }

    HDC dc = GetDC(hwnd);
    if (!dc)
        return BASE_DPI;

    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : BASE_DPI;
}

static int ScaleForDpi(int value)
{
    return MulDiv(value, static_cast<int>(g_dpi), BASE_DPI);
}

static UINT GetSystemDpi()
{
    HDC dc = GetDC(nullptr);
    if (!dc)
        return BASE_DPI;

    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : BASE_DPI;
}

static HFONT CreateControlFont(UINT dpi)
{
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);

    using SystemParametersInfoForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static const auto systemParametersInfoForDpi =
        reinterpret_cast<SystemParametersInfoForDpiFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));

    if (systemParametersInfoForDpi) {
        if (!systemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,
                                        sizeof(metrics), &metrics, 0, dpi)) {
            return nullptr;
        }
    } else {
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,
                                   sizeof(metrics), &metrics, 0)) {
            return nullptr;
        }

        // Win7/8.1 expose only the system-DPI metrics API. Scale its font
        // when a per-monitor-aware window is on a different-DPI monitor.
        UINT systemDpi = GetSystemDpi();
        if (dpi != systemDpi) {
            metrics.lfMessageFont.lfHeight = MulDiv(
                metrics.lfMessageFont.lfHeight, static_cast<int>(dpi), systemDpi);
            metrics.lfMessageFont.lfWidth = MulDiv(
                metrics.lfMessageFont.lfWidth, static_cast<int>(dpi), systemDpi);
        }
    }

    return CreateFontIndirectW(&metrics.lfMessageFont);
}

static void UpdateControlFont()
{
    HFONT newFont = CreateControlFont(g_dpi);
    if (!newFont)
        return;

    const HWND controls[] = {
        g_hwndBack,
        g_hwndForward,
        g_hwndRefresh,
        g_hwndStop,
        g_hwndAddress,
        g_hwndGo,
        g_hwndStatus,
    };

    for (HWND control : controls) {
        if (control)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);
    }

    HFONT oldFont = g_controlFont;
    g_controlFont = newFont;
    if (oldFont)
        DeleteObject(oldFont);
}

static HICON LoadApplicationIcon(HINSTANCE hInstance, int width, int height)
{
    return static_cast<HICON>(LoadImageW(
        hInstance,
        MAKEINTRESOURCEW(IDI_FLASHIE_ICON),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR | LR_SHARED));
}

static void DoNavigate()
{
    wchar_t url[2048];
    GetWindowTextW(g_hwndAddress, url, _countof(url));
    if (url[0] && g_pBrowser)
        g_pBrowser->Navigate(url);
}

static void OnNavigateComplete(const wchar_t* url, void*)
{
    SetWindowTextW(g_hwndAddress, url);
}

static void OnTitleChange(const wchar_t* title, void*)
{
    wchar_t buf[512];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s - FlashIE", title);
    SetWindowTextW(g_hwndMain, buf);
}

static void OnStatusTextChange(const wchar_t* text, void*)
{
    if (g_hwndStatus)
        SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text));
}

static void OnLoadingStateChange(bool isLoading, void*)
{
    if (g_hwndRefresh == nullptr || g_hwndStop == nullptr)
        return;
    if (isLoading) {
        ShowWindow(g_hwndStop, SW_SHOW);
        ShowWindow(g_hwndRefresh, SW_HIDE);
    } else {
        ShowWindow(g_hwndRefresh, SW_SHOW);
        ShowWindow(g_hwndStop, SW_HIDE);
    }
}

static void LayoutControls(int cx, int cy)
{
    const int toolbarHeight = ScaleForDpi(TOOLBAR_HEIGHT_DIP);
    const int buttonWidth = ScaleForDpi(BUTTON_WIDTH_DIP);
    const int buttonHeight = ScaleForDpi(BUTTON_HEIGHT_DIP);
    const int margin = ScaleForDpi(MARGIN_DIP);

    int statusHeight = 0;

    if (g_hwndStatus) {
        SendMessageW(g_hwndStatus, WM_SIZE, 0, 0);
        RECT rcStatus;
        if (GetWindowRect(g_hwndStatus, &rcStatus))
            statusHeight = rcStatus.bottom - rcStatus.top;
    }

    int x = margin;
    int y = (toolbarHeight - buttonHeight) / 2;

    MoveWindow(g_hwndBack,    x, y, buttonWidth, buttonHeight, TRUE); x += buttonWidth + margin;
    MoveWindow(g_hwndForward, x, y, buttonWidth, buttonHeight, TRUE); x += buttonWidth + margin;
    MoveWindow(g_hwndRefresh, x, y, buttonWidth, buttonHeight, TRUE);
    MoveWindow(g_hwndStop,    x, y, buttonWidth, buttonHeight, TRUE); x += buttonWidth + margin;

    int goX = cx - margin - buttonWidth;
    int addrWidth = goX - margin - x;

    if (addrWidth < ScaleForDpi(50)) {
        addrWidth = ScaleForDpi(50);
    }

    MoveWindow(g_hwndAddress, x, y, addrWidth, buttonHeight, TRUE);
    MoveWindow(g_hwndGo, goX, y, buttonWidth, buttonHeight, TRUE);

    if (g_pBrowser) {
        RECT rc = {0, toolbarHeight, cx, cy - statusHeight};
        g_pBrowser->Resize(rc);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = reinterpret_cast<LPCREATESTRUCT>(lParam)->hInstance;
        g_dpi = GetWindowDpi(hwnd);
        RECT rc;
        GetClientRect(hwnd, &rc);

        g_hwndBack    = CreateWindowW(L"BUTTON", L"Back",    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_BACK),    hInst, nullptr);
        g_hwndForward = CreateWindowW(L"BUTTON", L"Forward", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_FORWARD), hInst, nullptr);
        g_hwndRefresh = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_REFRESH), hInst, nullptr);
        g_hwndStop    = CreateWindowW(L"BUTTON", L"Stop",    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_STOP),    hInst, nullptr);
        g_hwndGo      = CreateWindowW(L"BUTTON", L"Go",      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_GO),      hInst, nullptr);
        g_hwndStatus  = CreateWindowW(STATUSCLASSNAMEW, L"Ready", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_STATUS), hInst, nullptr);
        g_hwndAddress = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_ADDRESS), hInst, nullptr);

        UpdateControlFont();
        OnLoadingStateChange(false, nullptr);

        // Install hooks BEFORE browser creation so COM/registry/security
        // hooks are in place when mshtml.dll initializes.
        // InstallHooks force-loads mshtml.dll/urlmon.dll/ieframe.dll.
        if (g_flashActivated && !FlashLoader::InstallHooks()) {
            MessageBoxW(hwnd, L"Failed to install the Flash compatibility hooks.\n"
                        L"Flash support has been disabled for this session.",
                        L"FlashIE", MB_ICONWARNING);
            FlashLoader::Deactivate();
            g_flashActivated = false;
        }

        g_pBrowser = new BrowserHost();
        RECT rcBrowser = {0, ScaleForDpi(TOOLBAR_HEIGHT_DIP), rc.right, rc.bottom};
        if (g_pBrowser->Initialize(hwnd, rcBrowser)) {
            g_pBrowser->SetNavigateCompleteCallback(OnNavigateComplete, nullptr);
            g_pBrowser->SetTitleChangeCallback(OnTitleChange, nullptr);
            g_pBrowser->SetStatusTextChangeCallback(OnStatusTextChange, nullptr);
            g_pBrowser->SetLoadingStateCallback(OnLoadingStateChange, nullptr);
            g_pBrowser->Navigate(g_initialAddress.c_str());
        }

        LayoutControls(rc.right, rc.bottom);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = ScaleForDpi(WINDOW_MIN_CX_DIP);
        mmi->ptMinTrackSize.y = ScaleForDpi(WINDOW_MIN_CY_DIP);
        return 0;
    }

    case WM_DPICHANGED: {
        UINT newDpi = HIWORD(wParam);
        g_dpi = newDpi != 0 ? newDpi : BASE_DPI;
        UpdateControlFont();

        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr,
                     suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);

        RECT rc;
        if (GetClientRect(hwnd, &rc))
            LayoutControls(rc.right, rc.bottom);
        return 0;
    }

    case WM_SIZE: {
        LayoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;
    }

    case WM_ACTIVATE:
        if (g_pBrowser) {
            g_pBrowser->OnFrameWindowActivate(
                LOWORD(wParam) != WA_INACTIVE);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_BACK:    if (g_pBrowser) g_pBrowser->GoBack();    break;
        case IDC_FORWARD: if (g_pBrowser) g_pBrowser->GoForward(); break;
        case IDC_REFRESH: if (g_pBrowser) g_pBrowser->Refresh();   break;
        case IDC_STOP:    if (g_pBrowser) g_pBrowser->Stop();      break;
        case IDC_GO:      DoNavigate(); break;
        }
        return 0;
    }

    case WM_PARENTNOTIFY:
        if (!g_isClosing && LOWORD(wParam) == WM_DESTROY) {
            HWND hwndDestroyed = reinterpret_cast<HWND>(lParam);
            if (g_pBrowser && hwndDestroyed == g_pBrowser->GetBrowserWindow()) {
                g_isClosing = true;
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;

    case WM_CLOSE:
        g_isClosing = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_pBrowser) {
            g_pBrowser->Destroy();
            delete g_pBrowser;
            g_pBrowser = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc > 1)
            g_initialAddress = argv[1];
        LocalFree(argv);
    }

    OleInitialize(nullptr);

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    // Register Flash.ocx class factory into this process's COM table.
    // Must be after OleInitialize (COM needs to be initialized).
    g_flashActivated = FlashLoader::Activate();
    if (!g_flashActivated) {
        MessageBoxW(nullptr, L"Failed to load Flash.ocx.\n"
                    L"Ensure Flash.ocx is alongside the executable.",
                    L"FlashIE", MB_ICONWARNING);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"FlashIEWindow";
    wc.hIcon         = LoadApplicationIcon(hInstance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm       = LoadApplicationIcon(hInstance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (!wc.hIcon)
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm)
        wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    g_hwndMain = CreateWindowExW(0, L"FlashIEWindow", L"FlashIE",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Address bar keyboard shortcuts
        if (msg.hwnd == g_hwndAddress && msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) {
                DoNavigate();
                continue;
            }
            if (msg.wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                SendMessageW(g_hwndAddress, EM_SETSEL, 0, -1);
                continue;
            }
        }
        // The browser host validates whether this keyboard message belongs
        // to its current window hierarchy before offering it to OLE.
        if (g_pBrowser && g_pBrowser->TranslateAccelerator(&msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_controlFont) {
        DeleteObject(g_controlFont);
        g_controlFont = nullptr;
    }

    // Deactivate Flash BEFORE OleUninitialize
    FlashLoader::Deactivate();
    g_flashActivated = false;
    OleUninitialize();

    return static_cast<int>(msg.wParam);
}
