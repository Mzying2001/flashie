#include "window_theme.h"

#include <commctrl.h>
#include <algorithm>
#include <string>

#include "debug.h"

namespace {

constexpr UINT BASE_DPI = 96;
constexpr UINT_PTR STATUS_SUBCLASS_ID = 1;
constexpr COLORREF BACKGROUND_COLOR = RGB(0x20, 0x20, 0x20);
constexpr COLORREF INPUT_COLOR = RGB(0x2B, 0x2B, 0x2B);
constexpr COLORREF TEXT_COLOR = RGB(0xF0, 0xF0, 0xF0);
constexpr COLORREF DISABLED_TEXT_COLOR = RGB(0x9D, 0x9D, 0x9D);

enum class PreferredAppMode {
    Default,
    AllowDark,
};

using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
using ShouldAppsUseDarkModeFn = bool(WINAPI*)();
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
using AllowDarkModeForAppFn = bool(WINAPI*)(bool);
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using RefreshImmersiveColorPolicyStateFn = void(WINAPI*)();
using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
using IsThemeActiveFn = BOOL(WINAPI*)();
using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

struct ThemeState {
    bool initialized = false;
    bool supported = false;
    bool appOptedIn = false;
    bool applying = false;
    bool dark = false;
    UINT dpi = BASE_DPI;
    WindowTheme::Controls controls;
    HMODULE uxtheme = nullptr;
    HMODULE dwmapi = nullptr;
    HBRUSH background = nullptr;
    HBRUSH input = nullptr;
    ShouldAppsUseDarkModeFn shouldAppsUseDarkMode = nullptr;
    AllowDarkModeForWindowFn allowDarkModeForWindow = nullptr;
    AllowDarkModeForAppFn allowDarkModeForApp = nullptr;
    SetPreferredAppModeFn setPreferredAppMode = nullptr;
    RefreshImmersiveColorPolicyStateFn refreshImmersiveColorPolicyState = nullptr;
    SetWindowThemeFn setWindowTheme = nullptr;
    IsThemeActiveFn isThemeActive = nullptr;
    IsThemeActiveFn isAppThemed = nullptr;
    DwmSetWindowAttributeFn dwmSetWindowAttribute = nullptr;
};

ThemeState g_theme;

HMODULE LoadSystemLibrary(const wchar_t* name)
{
    // LOAD_LIBRARY_SEARCH_SYSTEM32 itself requires an update on Windows 7.
    // An absolute system-directory path also works on unpatched older systems.
    wchar_t directory[MAX_PATH];
    UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return nullptr;

    std::wstring path(directory, length);
    path += L'\\';
    path += name;
    return LoadLibraryW(path.c_str());
}

int Scale(int value)
{
    return MulDiv(value, static_cast<int>(g_theme.dpi), BASE_DPI);
}

bool WantsDarkMode()
{
    HIGHCONTRASTW contrast = {};
    contrast.cbSize = sizeof(contrast);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) ||
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        return false;
    }

    return g_theme.isThemeActive() && g_theme.isAppThemed() &&
           g_theme.shouldAppsUseDarkMode();
}

void PaintStatus(HWND hwnd, HDC dc)
{
    int saved = SaveDC(dc);
    if (!saved)
        return;

    RECT client = {};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, g_theme.background);

    RECT separator = client;
    separator.bottom = separator.top + Scale(1);
    SetDCBrushColor(dc, RGB(0x3F, 0x3F, 0x3F));
    FillRect(dc, &separator, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    bool hasGrip = (GetWindowLongPtrW(hwnd, GWL_STYLE) & SBARS_SIZEGRIP) != 0 &&
                   !IsZoomed(GetParent(hwnd));
    int gripWidth = hasGrip ? Scale(17) : 0;

    // Keep the status control's text storage and SB_SETTEXT interface intact.
    // FlashIE currently uses one ordinary (non-owner-drawn) status part.
    LRESULT textInfo = SendMessageW(hwnd, SB_GETTEXTLENGTHW, 0, 0);
    if (!(HIWORD(textInfo) & SBT_OWNERDRAW)) {
        std::wstring text(static_cast<size_t>(LOWORD(textInfo)) + 1, L'\0');
        SendMessageW(hwnd, SB_GETTEXTW, 0, reinterpret_cast<LPARAM>(text.data()));

        RECT bounds = client;
        SendMessageW(hwnd, SB_GETRECT, 0, reinterpret_cast<LPARAM>(&bounds));
        bounds.left += Scale(4);
        bounds.top += Scale(1);
        bounds.right = (std::min)(bounds.right, client.right - gripWidth - Scale(2));

        HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
        SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, TEXT_COLOR);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text.c_str(), -1, &bounds,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    if (hasGrip) {
        // Only the glyph is drawn here; the native status bar still owns its
        // sizing hit tests and forwards the resize operation to the frame.
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, RGB(0x80, 0x80, 0x80));
        for (int offset = 4; offset <= 10; offset += 3) {
            MoveToEx(dc, client.right - Scale(offset), client.bottom - Scale(3), nullptr);
            LineTo(dc, client.right - Scale(3), client.bottom - Scale(offset));
        }
    }

    RestoreDC(dc, saved);
}

LRESULT CALLBACK StatusSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR)
{
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, StatusSubclassProc, subclassId);
        if (g_theme.controls.status == hwnd)
            g_theme.controls.status = nullptr;
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    if (g_theme.dark) {
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = BeginPaint(hwnd, &paint);
            if (dc)
                PaintStatus(hwnd, dc);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            if (wParam)
                PaintStatus(hwnd, reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_ERASEBKGND: {
            RECT client = {};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, g_theme.background);
            return 1;
        }
        case SB_SETTEXTA:
        case SB_SETTEXTW: {
            LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return result;
        }
        }
    }

    return DefSubclassProc(hwnd, message, wParam, lParam);
}

bool ApplyControlThemes(bool dark)
{
    bool success = true;
    g_theme.allowDarkModeForWindow(g_theme.controls.window, dark);
    for (HWND button : g_theme.controls.buttons) {
        if (!button)
            continue;
        g_theme.allowDarkModeForWindow(button, dark);
        if (FAILED(g_theme.setWindowTheme(button, dark ? L"Explorer" : nullptr, nullptr)))
            success = false;
    }

    if (g_theme.controls.address) {
        g_theme.allowDarkModeForWindow(g_theme.controls.address, dark);
        if (FAILED(g_theme.setWindowTheme(g_theme.controls.address,
                                         dark ? L"DarkMode_CFD" : nullptr, nullptr))) {
            success = false;
        }
    }
    return success;
}

void RedrawControl(HWND hwnd)
{
    if (hwnd)
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
}

void RefreshTheme()
{
    if (!g_theme.supported || !g_theme.controls.window || g_theme.applying)
        return;

    // SetWindowTheme sends WM_THEMECHANGED synchronously. Do not re-enter this
    // update or forward another theme broadcast to the browser hierarchy.
    g_theme.applying = true;
    g_theme.refreshImmersiveColorPolicyState();
    g_theme.dark = WantsDarkMode();
    if (!ApplyControlThemes(g_theme.dark) && g_theme.dark) {
        DbgTrace(L"[FlashIE] Native control theme unavailable; using the default theme.\n");
        g_theme.dark = false;
        ApplyControlThemes(false);
    }

    BOOL dark = g_theme.dark ? TRUE : FALSE;
    HRESULT result = g_theme.dwmSetWindowAttribute(
        g_theme.controls.window, 20, &dark, sizeof(dark));
    if (FAILED(result)) {
        // Windows 10 versions before 20H1 use attribute 19 instead of 20.
        result = g_theme.dwmSetWindowAttribute(
            g_theme.controls.window, 19, &dark, sizeof(dark));
    }
    if (FAILED(result))
        DbgTrace(L"[FlashIE] Dark title bar unavailable: 0x%08lX\n", result);

    RedrawControl(g_theme.controls.window);
    for (HWND button : g_theme.controls.buttons)
        RedrawControl(button);
    RedrawControl(g_theme.controls.address);
    RedrawControl(g_theme.controls.status);
    g_theme.applying = false;
}

} // namespace

namespace WindowTheme {

void Initialize()
{
    if (g_theme.initialized)
        return;
    g_theme.initialized = true;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!rtlGetVersion || rtlGetVersion(&version) != 0 ||
        version.dwMajorVersion != 10 || version.dwMinorVersion != 0 ||
        version.dwBuildNumber < 17763) {
        return;
    }

    g_theme.uxtheme = LoadSystemLibrary(L"uxtheme.dll");
    g_theme.dwmapi = LoadSystemLibrary(L"dwmapi.dll");
    if (!g_theme.uxtheme || !g_theme.dwmapi) {
        Shutdown();
        return;
    }

    g_theme.shouldAppsUseDarkMode = reinterpret_cast<ShouldAppsUseDarkModeFn>(
        GetProcAddress(g_theme.uxtheme, MAKEINTRESOURCEA(132)));
    g_theme.allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
        GetProcAddress(g_theme.uxtheme, MAKEINTRESOURCEA(133)));
    g_theme.refreshImmersiveColorPolicyState =
        reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(
            GetProcAddress(g_theme.uxtheme, MAKEINTRESOURCEA(104)));
    g_theme.setWindowTheme = reinterpret_cast<SetWindowThemeFn>(
        GetProcAddress(g_theme.uxtheme, "SetWindowTheme"));
    g_theme.isThemeActive = reinterpret_cast<IsThemeActiveFn>(
        GetProcAddress(g_theme.uxtheme, "IsThemeActive"));
    g_theme.isAppThemed = reinterpret_cast<IsThemeActiveFn>(
        GetProcAddress(g_theme.uxtheme, "IsAppThemed"));
    g_theme.dwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(g_theme.dwmapi, "DwmSetWindowAttribute"));

    // Ordinal 135 changed ABI in 1903. In particular, calling the enum-returning
    // function through the 1809 bool signature is unsafe on 32-bit builds.
    FARPROC appMode = GetProcAddress(g_theme.uxtheme, MAKEINTRESOURCEA(135));
    if (version.dwBuildNumber < 18362)
        g_theme.allowDarkModeForApp = reinterpret_cast<AllowDarkModeForAppFn>(appMode);
    else
        g_theme.setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(appMode);

    if (!g_theme.shouldAppsUseDarkMode || !g_theme.allowDarkModeForWindow ||
        !g_theme.refreshImmersiveColorPolicyState || !g_theme.setWindowTheme ||
        !g_theme.isThemeActive || !g_theme.isAppThemed ||
        !g_theme.dwmSetWindowAttribute || !appMode) {
        Shutdown();
        return;
    }

    g_theme.background = CreateSolidBrush(BACKGROUND_COLOR);
    g_theme.input = CreateSolidBrush(INPUT_COLOR);
    if (!g_theme.background || !g_theme.input) {
        Shutdown();
        return;
    }

    if (g_theme.setPreferredAppMode)
        g_theme.setPreferredAppMode(PreferredAppMode::AllowDark);
    else
        g_theme.allowDarkModeForApp(true);
    g_theme.appOptedIn = true;
    g_theme.supported = true;
}

void Attach(const Controls& controls, UINT dpi)
{
    if (!g_theme.supported || g_theme.controls.window || !controls.window)
        return;

    if (controls.status && !SetWindowSubclass(
            controls.status, StatusSubclassProc, STATUS_SUBCLASS_ID, 0)) {
        DbgTrace(L"[FlashIE] Could not subclass the status bar; using the default theme.\n");
        Shutdown();
        return;
    }

    g_theme.controls = controls;
    SetDpi(dpi);
    RefreshTheme();
}

void SetDpi(UINT dpi)
{
    g_theme.dpi = dpi ? dpi : BASE_DPI;
    if (g_theme.controls.status)
        InvalidateRect(g_theme.controls.status, nullptr, FALSE);
}

bool HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                   LRESULT& result)
{
    if (!g_theme.supported || hwnd != g_theme.controls.window)
        return false;

    switch (message) {
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        // Also covers high-contrast changes and notifications with no name.
        RefreshTheme();
        break;

    case WM_ERASEBKGND:
        if (g_theme.dark) {
            RECT client = {};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, g_theme.background);
            result = 1;
            return true;
        }
        break;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (g_theme.dark && reinterpret_cast<HWND>(lParam) == g_theme.controls.address) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, IsWindowEnabled(g_theme.controls.address)
                             ? TEXT_COLOR : DISABLED_TEXT_COLOR);
            SetBkColor(dc, INPUT_COLOR);
            result = reinterpret_cast<LRESULT>(g_theme.input);
            return true;
        }
        break;

    case WM_CTLCOLORBTN:
        if (g_theme.dark) {
            for (HWND button : g_theme.controls.buttons) {
                if (button && button == reinterpret_cast<HWND>(lParam)) {
                    SetBkColor(reinterpret_cast<HDC>(wParam), BACKGROUND_COLOR);
                    result = reinterpret_cast<LRESULT>(g_theme.background);
                    return true;
                }
            }
        }
        break;

    case WM_NCDESTROY:
        if (g_theme.controls.status && IsWindow(g_theme.controls.status))
            RemoveWindowSubclass(g_theme.controls.status, StatusSubclassProc,
                                 STATUS_SUBCLASS_ID);
        g_theme.controls = {};
        break;
    }
    return false;
}

void Shutdown()
{
    if (g_theme.controls.status && IsWindow(g_theme.controls.status))
        RemoveWindowSubclass(g_theme.controls.status, StatusSubclassProc, STATUS_SUBCLASS_ID);

    if (g_theme.appOptedIn) {
        if (g_theme.setPreferredAppMode)
            g_theme.setPreferredAppMode(PreferredAppMode::Default);
        else
            g_theme.allowDarkModeForApp(false);
    }
    if (g_theme.background)
        DeleteObject(g_theme.background);
    if (g_theme.input)
        DeleteObject(g_theme.input);
    if (g_theme.uxtheme)
        FreeLibrary(g_theme.uxtheme);
    if (g_theme.dwmapi)
        FreeLibrary(g_theme.dwmapi);
    g_theme = {};
}

} // namespace WindowTheme
