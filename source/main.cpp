#include <windows.h>
#include <ole2.h>
#include <shlwapi.h>
#include <stdio.h>
#include "flash_loader.h"
#include "browser.h"


static constexpr int TOOLBAR_HEIGHT = 32;
static constexpr int BUTTON_WIDTH   = 60;
static constexpr int BUTTON_HEIGHT  = 24;
static constexpr int MARGIN         = 4;

enum ControlID {
    ID_BACK = 1001,
    ID_FORWARD,
    ID_REFRESH,
    ID_STOP,
    ID_ADDRESS,
    ID_GO,
};

static FlashLoader  g_flashLoader;
static BrowserHost* g_pBrowser        = nullptr;
static HWND         g_hwndMain        = nullptr;
static HWND         g_hwndBrowserArea = nullptr;
static HWND         g_hwndBack        = nullptr;
static HWND         g_hwndForward     = nullptr;
static HWND         g_hwndRefresh     = nullptr;
static HWND         g_hwndStop        = nullptr;
static HWND         g_hwndAddress     = nullptr;
static HWND         g_hwndGo          = nullptr;

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

static void LayoutControls(int cx, int cy)
{
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

    MoveWindow(g_hwndBrowserArea, 0, TOOLBAR_HEIGHT, cx, cy - TOOLBAR_HEIGHT, TRUE);
    if (g_pBrowser) {
        RECT rc = {0, 0, cx, cy - TOOLBAR_HEIGHT};
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

        g_hwndBrowserArea = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, TOOLBAR_HEIGHT, rc.right, rc.bottom - TOOLBAR_HEIGHT,
            hwnd, nullptr, hInst, nullptr);

        g_pBrowser = new BrowserHost();
        RECT rcBrowser = {0, 0, rc.right, rc.bottom - TOOLBAR_HEIGHT};
        if (g_pBrowser->Initialize(g_hwndBrowserArea, rcBrowser)) {
            // Install hooks AFTER browser creation so mshtml/urlmon are loaded
            g_flashLoader.InstallHooks();

            g_pBrowser->SetNavigateCompleteCallback(OnNavigateComplete, nullptr);
            g_pBrowser->SetTitleChangeCallback(OnTitleChange, nullptr);
            g_pBrowser->Navigate(L"about:blank");
        }

        LayoutControls(rc.right, rc.bottom);
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
    OleInitialize(nullptr);

    // Register Flash.ocx class factory into this process's COM table.
    // Must be after OleInitialize (COM needs to be initialized).
    if (!g_flashLoader.Activate()) {
        MessageBoxW(nullptr, L"Failed to load Flash.ocx.\n"
                    L"Ensure Flash.ocx is in the application directory.",
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
        WS_OVERLAPPEDWINDOW,
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
            if (hwndFocus && (hwndFocus == g_hwndBrowserArea || IsChild(g_hwndBrowserArea, hwndFocus))) {
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
