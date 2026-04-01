# AGENTS.md

This file provides guidance to AI agents when working with code in this repository.

## Overview

FlashIE is a standalone Windows application that hosts an embedded IE WebBrowser control with a locally-loaded Adobe Flash Player ActiveX control (Flash.ocx). It uses a pre-patched Flash.ocx (no time bomb or region restrictions) and bypasses Windows/IE security policies to allow running Flash content without Flash being registered system-wide. Flash content auto-activates without user clicks, including in iframes.

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

Output binary: `build/<Config>/FlashIE.exe`. Post-build steps automatically copy the architecture-matched `Flash.ocx` to the output directory.

## Architecture

### Source Files

Four source files and four headers:

- **`source/flash_loader.h/.cpp`** — `FlashLoader` class. The core hooking engine, organized into 9 sections:
  - **Section 1-2**: Includes, constants, function pointer typedefs for all hooked APIs.
  - **Section 3**: Minimal x86/x64 instruction length decoder (`InsnLength`) for computing hook trampoline sizes.
  - **Section 4**: Inline hook (detour) infrastructure — `InstallDetour`/`RemoveDetour` with trampoline allocation via `VirtualAlloc`.
  - **Section 5**: Fake registry key system — tracks real `HKEY` handles (opened read-only) used as sentinels for fake Flash registry entries.
  - **Section 6**: COM wrapper classes:
    - `LoggingClassFactory` — wraps Flash's `IClassFactory`, hooks new Flash objects on `CreateInstance` (installs QI, SetClientSite, and QuickActivate vtable hooks).
    - `FlashSafetyTearoff` — `IObjectSafety` tearoff delegating COM identity to Flash (required by MSHTML for scripting).
    - `PropertyBagWrapper` — forces `allowScriptAccess="always"` in `IPropertyBag::Read` for ExternalInterface support.
    - `FlashPersistPBagTearoff` — wraps `IPersistPropertyBag::Load` to inject the PropertyBagWrapper.
  - **Section 7**: Shared static state (paths, factory pointers, fake registry handles).
  - **Section 8a**: COM hooks — `CoGetClassObject`, `CoCreateInstance`, `CoGetClassObjectFromURL`, `CLSIDFromProgID`. Intercept Flash CLSID requests and redirect to our local factory.
  - **Section 8b**: Registry hooks — `RegOpenKeyExW`, `RegQueryValueExW`, `RegCloseKey`. Fake Flash CLSID registration (InprocServer32, TypeLib, ProgID), MIME type mapping, ActiveX kill bit bypass, FEATURE_BROWSER_EMULATION.
  - **Section 8c**: Security hooks — `WldpIsClassInApprovedList`, `WldpQueryDynamicCodeTrust` (WLDP bypass), `CoInternetIsFeatureEnabled` (feature control bypass).
  - **Section 8d**: Flash `QueryInterface` vtable hook — injects `IObjectSafety` (via `FlashSafetyTearoff`) and `IPersistPropertyBag` (via `FlashPersistPBagTearoff`) into Flash objects.
  - **Section 8e**: Flash forced in-place activation — hooks `IOleObject::SetClientSite` and `IQuickActivate::QuickActivate` vtables. When MSHTML sets a client site, queues a 100ms timer to call `DoVerb(OLEIVERB_INPLACEACTIVATE)`. For `display:none` iframes, a 200ms repeating deferred timer re-activates Flash objects (max 50 retries / 10 seconds).
  - **Section 8f**: TypeLib hook (`LoadRegTypeLib`) — redirects Flash TypeLib GUID to `LoadTypeLibEx` on the local OCX file.
  - **Section 9**: Public API — `Activate()` (loads OCX, creates factory, registers with COM), `InstallHooks()` (force-loads IE DLLs, installs all detours), `Deactivate()` (flushes pending activations, removes hooks, revokes COM registration).

- **`source/browser.h/.cpp`** — `BrowserHost` and OLE site classes (`COleSite`, `COleClientSite`, `COleInPlaceSite`, `COleInPlaceFrame`). Implements the standard OLE container interfaces needed to host an `IWebBrowser2` (IE) control in-process. `COleSite` implements:
  - `IDocHostUIHandler` — disables 3D border (`DOCHOSTUIFLAG_NO3DBORDER`).
  - `IOleCommandTarget` — suppresses script error dialogs (`OLECMDID_SHOWSCRIPTERROR`).
  - `IDispatch` — `DWebBrowserEvents2` event sink for NavigateComplete2, TitleChange, NewWindow2/NewWindow3 (redirects new windows to same browser), and `DISPID_AMBIENT_DLCONTROL` ambient property (allows content downloads but blocks ActiveX CAB downloads via `DLCTL_NO_DLACTIVEXCTLS`).

- **`source/flash.h/.cpp`** — MIDL-generated Flash COM interface definitions (`IShockwaveFlash`, `CLSID_ShockwaveFlash`, `LIBID_ShockwaveFlashObjects`, etc.).

- **`source/debug.h`** — `DbgTrace` macro for diagnostic output.

- **`source/main.cpp`** — Win32 window with a toolbar (Back/Forward/Refresh/Stop/address bar/Go) and a browser area. Initialization order: `OleInitialize` → `FlashLoader::Activate()` → create window → `FlashLoader::InstallHooks()` (force-loads mshtml/urlmon/ieframe and installs COM/registry/security/TypeLib hooks) → create browser. Shutdown: `FlashLoader::Deactivate()` → `OleUninitialize`.

### Assets

- **`assets/Flash32.ocx`** / **`assets/Flash64.ocx`** — Pre-patched Flash Player ActiveX (32-bit / 64-bit).
- **`assets/app.manifest`** — Registration-Free COM declarations for Flash.ocx (SxS activation context).

## Core Requirement: Zero Registry Pollution

The program MUST NOT register Flash.ocx into the system registry, and MUST NOT write to or modify the system registry. All registry operations must be strictly process-local:

- **No `regsvr32` or `DllRegisterServer`**: Flash.ocx (alongside the exe) is loaded via `LoadLibrary` + `DllGetClassObject` only. The class factory is registered in-process via `CoRegisterClassObject`, never written to `HKCR` or `HKLM`.
- **No registry writes**: Registry hooks (`RegOpenKeyExW`, `RegQueryValueExW`) return fake in-memory responses for Flash CLSID lookups. No actual registry keys are created or modified. Fake `HKEY` handles are opened read-only on existing unrelated keys — used only as valid handle values, never written to.
- **Process-scoped hooks**: All inline hooks (COM, registry, WLDP, TypeLib) operate only within the current process's address space. They are removed on shutdown via `FlashLoader::Deactivate()`.

When adding new features or modifying hooks, ensure this invariant is preserved. Any code path that could write to the registry or register COM objects system-wide is a bug.

## Key Design Details

- **Inline hooking**: Patches the first bytes of target functions with a JMP to the hook, saving originals in a trampoline. Not IAT patching — intercepts all callers including vtable and `GetProcAddress`-resolved calls. Uses a custom x86/x64 instruction length decoder to determine how many bytes to copy for the trampoline prologue.
- **Fake registry keys**: Uses real `HKEY` handles (opened read-only on existing keys) tracked in a table, because Windows internals dereference `HKEY` as a pointer — sentinel values cause access violations.
- **`LoggingClassFactory`**: Wraps the real Flash class factory. On `CreateInstance`, installs three vtable hooks on the new Flash object: (1) `QueryInterface` hook for `IObjectSafety`/`IPersistPropertyBag` injection, (2) `IOleObject::SetClientSite` hook for forced activation, (3) `IQuickActivate::QuickActivate` hook for iframe activation. Registered as the COM class object (not the raw factory) so all creation paths go through it.
- **Forced activation**: Windows 10 defers Flash `DoVerb(INPLACEACTIVATE)` until user click. Two vtable hooks solve this:
  - `SetClientSite` hook — catches the normal MSHTML activation path.
  - `QuickActivate` hook — catches the alternative path used by cross-domain iframes.
  - Both queue a 100ms coalescing timer that calls `DoVerb`. After the initial activation, objects are saved into a deferred array with a 200ms repeating timer (max 50 retries) to handle `display:none` iframes that need re-activation when they become visible.

## Debugging

All diagnostic output uses `OutputDebugStringW` with `[FlashIE]` prefix. View with Visual Studio debugger Output window or Sysinternals DebugView. Debug builds additionally probe Flash objects for key interfaces on each `CreateInstance` call.
