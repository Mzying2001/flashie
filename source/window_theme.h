#pragma once

#include <windows.h>

// Themes only the application's own chrome, never the hosted browser windows.
// All functions run on the main window thread. Unsupported systems are no-ops.
namespace WindowTheme {

struct Controls {
    HWND window = nullptr;
    HWND address = nullptr;
    HWND status = nullptr;
    HWND buttons[4] = {};
};

void Initialize();
void Attach(const Controls& controls, UINT dpi);
void SetDpi(UINT dpi);
bool HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                   LRESULT& result);
void Shutdown();

} // namespace WindowTheme
