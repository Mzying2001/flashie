#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <string>
#include "flash_loader.h"
#include "browser.h"


static constexpr int TOOLBAR_HEIGHT = 32;
static constexpr int BUTTON_WIDTH   = 60;
static constexpr int BUTTON_HEIGHT  = 24;
static constexpr int MARGIN         = 4;
static constexpr int WINDOW_MIN_CX  = 500;
static constexpr int WINDOW_MIN_CY  = 350;

enum ControlID {
    ID_BACK = 1001,
    ID_FORWARD,
    ID_REFRESH,
    ID_STOP,
    ID_ADDRESS,
    ID_GO,
    ID_STATUS,
};

static FlashLoader  g_flashLoader;
static BrowserHost* g_pBrowser        = nullptr;
static HWND         g_hwndMain        = nullptr;
static HWND         g_hwndBack        = nullptr;
static HWND         g_hwndForward     = nullptr;
static HWND         g_hwndRefresh     = nullptr;
static HWND         g_hwndStop        = nullptr;
static HWND         g_hwndAddress     = nullptr;
static HWND         g_hwndGo          = nullptr;
static HWND         g_hwndStatus      = nullptr;
static bool         g_isClosing       = false;
static std::wstring g_initialAddress  = L"https://www.bing.com/";

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
    if (g_hwndRefresh)
        EnableWindow(g_hwndRefresh, !isLoading);
    if (g_hwndStop)
        EnableWindow(g_hwndStop, isLoading);
}

static void LayoutControls(int cx, int cy)
{
    int statusHeight = 0;
    if (g_hwndStatus) {
        SendMessageW(g_hwndStatus, WM_SIZE, 0, 0);
        RECT rcStatus;
        if (GetWindowRect(g_hwndStatus, &rcStatus))
            statusHeight = rcStatus.bottom - rcStatus.top;
    }

    int x = MARGIN;
    int y = (TOOLBAR_HEIGHT - BUTTON_HEIGHT) / 2;
    MoveWindow(g_hwndBack,    x, y, BUTTON_WIDTH, BUTTON_HEIGHT, TRUE); x += BUTTON_WIDTH + MARGIN;
    MoveWindow(g_hwndForward, x, y, BUTTON_WIDTH, BUTTON_HEIGHT, TRUE); x += BUTTON_WIDTH + MARGIN;
    MoveWindow(g_hwndRefresh, x, y, BUTTON_WIDTH, BUTTON_HEIGHT, TRUE); x += BUTTON_WIDTH + MARGIN;
    MoveWindow(g_hwndStop,    x, y, BUTTON_WIDTH, BUTTON_HEIGHT, TRUE); x += BUTTON_WIDTH + MARGIN;

    int goX = cx - MARGIN - BUTTON_WIDTH;
    int addrWidth = goX - MARGIN - x;
    if (addrWidth < 50) addrWidth = 50;
    MoveWindow(g_hwndAddress, x, y, addrWidth, BUTTON_HEIGHT, TRUE);
    MoveWindow(g_hwndGo, goX, y, BUTTON_WIDTH, BUTTON_HEIGHT, TRUE);

    if (g_pBrowser) {
        RECT rc = {0, TOOLBAR_HEIGHT, cx, cy - statusHeight};
        g_pBrowser->Resize(rc);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = reinterpret_cast<LPCREATESTRUCT>(lParam)->hInstance;
        RECT rc;
        GetClientRect(hwnd, &rc);

        g_hwndBack    = CreateWindowW(L"BUTTON", L"Back",    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_BACK),    hInst, nullptr);
        g_hwndForward = CreateWindowW(L"BUTTON", L"Forward", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_FORWARD), hInst, nullptr);
        g_hwndRefresh = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_REFRESH), hInst, nullptr);
        g_hwndStop    = CreateWindowW(L"BUTTON", L"Stop",    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_STOP),    hInst, nullptr);
        g_hwndAddress = CreateWindowW(L"EDIT",   L"",        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_ADDRESS), hInst, nullptr);
        g_hwndGo      = CreateWindowW(L"BUTTON", L"Go",      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_GO),      hInst, nullptr);
        g_hwndStatus  = CreateWindowW(STATUSCLASSNAMEW, L"Ready", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(ID_STATUS), hInst, nullptr);

        OnLoadingStateChange(false, nullptr);

        // Install hooks BEFORE browser creation so COM/registry/security
        // hooks are in place when mshtml.dll initializes.
        // InstallHooks force-loads mshtml.dll/urlmon.dll/ieframe.dll.
        if (!g_flashLoader.InstallHooks() && g_flashLoader.IsActive()) {
            MessageBoxW(hwnd, L"Failed to install the Flash compatibility hooks.\n"
                        L"Flash support has been disabled for this session.",
                        L"FlashIE", MB_ICONWARNING);
            g_flashLoader.Deactivate();
        }

        g_pBrowser = new BrowserHost();
        RECT rcBrowser = {0, TOOLBAR_HEIGHT, rc.right, rc.bottom};
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
        mmi->ptMinTrackSize.x = WINDOW_MIN_CX;
        mmi->ptMinTrackSize.y = WINDOW_MIN_CY;
        return 0;
    }

    case WM_SIZE: {
        LayoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_BACK:    if (g_pBrowser) g_pBrowser->GoBack();    break;
        case ID_FORWARD: if (g_pBrowser) g_pBrowser->GoForward(); break;
        case ID_REFRESH: if (g_pBrowser) g_pBrowser->Refresh();   break;
        case ID_STOP:    if (g_pBrowser) g_pBrowser->Stop();      break;
        case ID_GO:      DoNavigate(); break;
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
    if (!g_flashLoader.Activate()) {
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
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwndMain = CreateWindowExW(0, L"FlashIEWindow", L"FlashIE",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
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
        // Let WebBrowser handle keyboard input only when focus is in browser area
        if (g_pBrowser) {
            HWND hwndFocus = GetFocus();
            HWND hwndBrowser = g_pBrowser->GetBrowserWindow();
            if (hwndFocus && hwndBrowser &&
                (hwndFocus == hwndBrowser || IsChild(hwndBrowser, hwndFocus))) {
                if (g_pBrowser->TranslateAccelerator(&msg))
                    continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Deactivate Flash BEFORE OleUninitialize
    g_flashLoader.Deactivate();
    OleUninitialize();

    return static_cast<int>(msg.wParam);
}
