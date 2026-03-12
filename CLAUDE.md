# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

FlashIE is a standalone Windows application that hosts an embedded IE WebBrowser control with a locally-loaded Adobe Flash Player ActiveX control (Flash.ocx). It bypasses Flash's end-of-life kill switch and Windows security policies to allow running Flash content without Flash being registered system-wide.

## Build

CMake project targeting MSVC with C++17 and static CRT (`/MT` for Release, `/MTd` for Debug).

```bash
# Generate build files (from repo root)
cmake -S . -B build -A Win32    # 32-bit
cmake -S . -B build -A x64      # 64-bit

# Build
cmake --build build --config Debug
cmake --build build --config Release
```

Output binary: `build/<Config>/FlashIE.exe`. Post-build steps automatically copy the architecture-matched `Flash.ocx` and `mms.cfg` to the output directory.

## Architecture

Three source files, each with a distinct responsibility:

- **`source/flash_loader.h/.cpp`** — `FlashLoader` class. Loads `Flash.ocx` from the exe directory via `LoadLibrary` + `DllGetClassObject`, registers its class factory into COM, then installs inline function hooks (detours) on COM APIs (`CoGetClassObject`, `CoCreateInstance`, `CoGetClassObjectFromURL`), registry APIs (`RegOpenKeyExW`, `RegQueryValueExW`, `RegCloseKey`), time APIs (`GetLocalTime`, `GetSystemTime`), and Windows security APIs (`WldpIsClassInApprovedList`, `WldpQueryDynamicCodeTrust`, `CoInternetIsFeatureEnabled`). The hooks intercept requests for the Flash CLSID (`{D27CDB6E-AE6D-11CF-96B8-444553540000}`) and redirect them to the locally-loaded factory, fake registry entries for Flash's InprocServer32/TypeLib/ProgID, and spoof system time to before Flash's EOL date. Contains a minimal x86/x64 instruction length decoder for computing hook trampoline sizes.

- **`source/browser.h/.cpp`** — `BrowserHost` and OLE site classes (`COleSite`, `COleClientSite`, `COleInPlaceSite`, `COleInPlaceFrame`). Implements the standard OLE container interfaces needed to host an `IWebBrowser2` (IE) control in-process. `COleSite` implements `IDocHostUIHandler` (no 3D border) and a `DWebBrowserEvents2` event sink (navigation callbacks, title changes, new-window redirection). Security and ActiveX approval for Flash are handled entirely by process-level hooks in `flash_loader.cpp`, not by the browser host.

- **`source/main.cpp`** — Win32 window with a toolbar (Back/Forward/Refresh/Stop/address bar/Go) and a browser area. Initialization order: `OleInitialize` → `FlashLoader::Activate()` → create window/browser → `FlashLoader::InstallHooks()` (must be after browser creation so mshtml/urlmon DLLs are loaded). Shutdown: `FlashLoader::Deactivate()` → `OleUninitialize`.

## Core Requirement: Zero System Pollution

The program MUST NOT register Flash.ocx into the system, and MUST NOT write to or modify the system registry or any system files. All operations must be strictly process-local:

- **No `regsvr32` or `DllRegisterServer`**: Flash.ocx is loaded via `LoadLibrary` + `DllGetClassObject` only. The class factory is registered in-process via `CoRegisterClassObject`, never written to `HKCR` or `HKLM`.
- **No registry writes**: Registry hooks (`RegOpenKeyExW`, `RegQueryValueExW`) return fake in-memory responses for Flash CLSID lookups. No actual registry keys are created or modified. Fake `HKEY` handles are opened read-only on existing unrelated keys — used only as valid handle values, never written to.
- **No system file modifications**: Flash.ocx and mms.cfg live in the application directory alongside the exe. Flash SharedObject data (`FlashData/`) is redirected to the application directory via file system hooks, not to `%APPDATA%`.
- **Process-scoped hooks**: All inline hooks (COM, registry, time, WLDP) operate only within the current process's address space. They are removed on shutdown via `FlashLoader::Deactivate()`.

When adding new features or modifying hooks, ensure this invariant is preserved. Any code path that could write to the registry or register COM objects system-wide is a bug.

## Key Design Details

- **Inline hooking**: Patches the first bytes of target functions with a JMP to the hook, saving originals in a trampoline. Not IAT patching — intercepts all callers including vtable and `GetProcAddress`-resolved calls.
- **Fake registry keys**: Uses real `HKEY` handles (opened read-only on existing keys) tracked in a table, because Windows internals dereference `HKEY` as a pointer — sentinel values cause access violations.
- **`LoggingClassFactory`**: Wraps the real Flash class factory to log `CreateInstance` calls and probe interface support. Needed because MSHTML calls `pCF->CreateInstance()` directly on the vtable, bypassing COM hooks.
- **`mms.cfg`**: Flash configuration file that disables EOL uninstall, auto-update, and the allowlist check.
## Debugging

All diagnostic output uses `OutputDebugStringW` with `[FlashIE]` prefix. View with Visual Studio debugger Output window or Sysinternals DebugView.
