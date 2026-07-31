# AGENTS.md

This file provides guidance to AI agents when working with code in this repository.

## Overview

FlashIE is a standalone Windows application that hosts an embedded IE WebBrowser control with a locally-loaded Adobe Flash Player ActiveX control (Flash.ocx). It uses a pre-patched Flash.ocx (no time bomb or region restrictions) and applies process-local, Flash-scoped compatibility exceptions to run content without registering Flash system-wide. Flash activation is forced without user clicks, with bounded retries for hidden iframes. Top-level HTTP(S) and local `.swf` navigations are converted in-process into full-window Flash documents while preserving the original navigation URL.

## Build

CMake project targeting MSVC with C++17 and static CRT (`/MT` for Release, `/MTd` for Debug).

```bash
# Generate build files (from repo root)
cmake -S . -B build-x86 -A Win32    # 32-bit
cmake -S . -B build-x64 -A x64      # 64-bit

# Windows 7 target (selects the OCX for Windows 7 and earlier)
cmake -S . -B build-win7-x86 -A Win32 -DFLASHIE_WINDOWS_TARGET=WIN7
cmake -S . -B build-win7-x64 -A x64 -DFLASHIE_WINDOWS_TARGET=WIN7

# Build (replace Release with Debug when needed)
cmake --build build-x86 --config Release
cmake --build build-x64 --config Release
cmake --build build-win7-x86 --config Release
cmake --build build-win7-x64 --config Release
```

Output binary: `<build-directory>/<Config>/FlashIE.exe`. Post-build steps automatically copy the selected architecture- and OS-matched `Flash.ocx` to the output directory.

## Architecture

### Source Files

Application sources and headers, plus three submodule dependencies:

- **`Detours/`** — [Microsoft Detours](https://github.com/microsoft/Detours.git) submodule. A library for intercepting Win32 API function calls. Built as a static library (`detours`) in CMake and linked into flashie. Used for all API-level inline hooks (COM, registry, file, host identity, WLDP, TypeLib).
- **`JScriptCC/`** — [JScriptCC](https://github.com/Mzying2001/JScriptCC.git) submodule. A C++ library for JScript Conditional Compilation preprocessing (`@cc_on`, `@if`, `@set`, `@end`). Built as a static library (`jscriptcc`) and linked into flashie. Used by the `ParseScriptText` hook to expand CC blocks before script execution.
- **`swc-es5-c-api/`** — [swc-es5-c-api](https://github.com/Mzying2001/swc-es5-c-api.git) submodule. A Rust static library exposing a C ABI for transpiling classic JavaScript to ES5. CMake builds its architecture-matched Win7 MSVC target through Cargo and links it as `swc_es5`. The `ParseScriptText` hook invokes it after JScriptCC for both classic JScript and JScript9 engines; expression-mode calls bypass SWC because its input contract is a complete script.
- **`source/flash_loader.h/.cpp`** — Process-wide `FlashLoader` namespace. The core hooking engine is exposed through `Activate`, `InstallHooks`, and `Deactivate`, and is organized into 8 sections:
  - **Section 1-2**: Includes, constants, function pointer typedefs for all hooked APIs.
  - **Section 3**: Process-wide state — one `LoaderState` owns the lifecycle phase, activation STA, module/factory/COM cookie, API Detours pointers, fake keys, Flash hooks, script hooks, and activation timers. Independent SRW locks protect lifecycle transitions and the factory, fake-key, Flash-hook, and script-hook domains.
  - **Section 4**: API hook target resolution — `ResolveTarget` resolves API exports with module fallbacks, while `ResetApiHookPointers` clears the `LoaderState::api` table after installation failure or successful detachment. `InstallHooks()` consumes these persistent pointer slots for its atomic Detours transaction.
  - **Section 5**: Fake registry key system — an SRW-lock-protected table tracks real `HKEY` handles opened read-only as sentinels for fake Flash entries, plus real `FEATURE_BROWSER_EMULATION` handles whose executable value is overlaid in memory. Query buffer sizes follow Win32 registry contracts, and every tracked handle is closed on normal close or shutdown.
  - **Section 6**: COM wrapper classes:
    - `LoggingClassFactory` — wraps Flash's `IClassFactory` and hooks new Flash objects on `CreateInstance`: a Detours hook on Flash `QueryInterface`, plus direct vtable hooks on SetClientSite and QuickActivate.
    - `FlashSafetyTearoff` — `IObjectSafety` tearoff with an independent interlocked lifetime that delegates COM identity to Flash (required by MSHTML for scripting). Page-provided property-bag values such as `allowScriptAccess` are not overridden.
  - **Section 7a**: COM hooks — `CoGetClassObject`, `CoCreateInstance`, `CoGetClassObjectFromURL`, `CLSIDFromProgID`. Intercept Flash CLSID requests and redirect to our local factory.
  - **Section 7b**: Registry hooks — `RegOpenKeyExW`, `RegQueryValueExW`, `RegCloseKey`. Provide fake Flash CLSID registration (InprocServer32, TypeLib, ProgID), MIME type mapping, scoped ActiveX kill-bit bypass, and a `FEATURE_BROWSER_EMULATION` value. Flash CLSID and ProgID paths use precise, case-insensitive element matching; unrelated `Compatibility Flags` values are never rewritten.
  - **Section 7c**: Security hooks — `WldpIsClassInApprovedList` approves only the Flash CLSID, while `WldpQueryDynamicCodeTrust` trusts only the locally loaded `Flash.ocx` file or image. Other classes and code retain the system WLDP result. WLDP is optional on older Windows: the pair is installed only when both exports are available.
  - **Section 7d**: Flash `QueryInterface` hook — injects only `IObjectSafety` via `FlashSafetyTearoff`; Flash's normal `IPersistPropertyBag` behavior and page-controlled properties remain untouched.
  - **Section 7e**: Flash forced in-place activation — hooks `IOleObject::SetClientSite` and `IQuickActivate::QuickActivate` vtables. Their callbacks feed a shared, object-deduplicated initial queue only on the `Activate()` owning STA. A coalesced 100ms timer obtains a valid parent HWND and position rectangle from `IOleInPlaceSite` before calling `DoVerb(OLEIVERB_INPLACEACTIVATE)`. Only objects with an unavailable site, window, or rectangle, or a failed `DoVerb`, enter the 200ms deferred retry queue (max 50 retries / 10 seconds); successful objects are removed immediately and are not re-activated. Both vtable slots are restored during deactivation.
  - **Section 7f**: TypeLib hook (`LoadRegTypeLib`) — redirects Flash TypeLib GUID to `LoadTypeLibEx` on the local OCX file.
  - **Section 7g**: Script engine `ParseScriptText` vtable hook — uses the SDK-selected `IID_IActiveScriptParse` for Win32/x64 and hooks only the classic JScript and JScript9 CLSIDs, never VBScript. Both engines convert the incoming UTF-16 code to UTF-8 once, preprocess Conditional Compilation via JScriptCC (`@cc_on` / `@if` / `@set` / `@end`), and then transpile complete scripts to ES5 through the `swc-es5-c-api` C ABI without an intermediate encoding round trip. The final result is converted back to UTF-16 for JScript. Expression-mode calls use only JScriptCC. A failed stage falls back to the latest valid code. Multiple engine vtables and their original methods are tracked under an SRW lock and restored during shutdown.
  - **Section 7h**: Flash browser-host identity — scoped `GetModuleFileNameA/W` hooks report an `iexplore.exe` basename only when the local `Flash.ocx` queries the current process (`hModule == nullptr`). Flash otherwise treats a third-party WebBrowser host as non-browser content and drops `navigateToURL` before calling `IBindHost` or URLMon. The real directory is preserved; all non-Flash callers and explicit module queries pass through unchanged.
  - **Section 7i**: SharedObject first-save compatibility — scoped `DeleteFileA/W` hooks treat `ERROR_FILE_NOT_FOUND` as success only when the local `Flash.ocx` deletes a `.sol`, allowing Flash to continue its own `.sxx` commit. All other file deletions pass through unchanged.
  - **Section 8**: Public namespace API and lifecycle — `Activate()` and `InstallHooks()` are idempotent on the owning STA. `Activate()` publishes state only after the OCX, wrapped factory, and process-local COM registration are ready. `InstallHooks()` atomically installs the COM/registry/file/host-identity/WLDP/TypeLib API hook set. `Deactivate()` enters a retryable cleanup phase, stops activation timers, restores every tracked vtable, detaches the API and Flash QI hooks transactionally, closes fake keys, revokes the process-local class object, and releases the factories and loader-owned module reference. Failed restoration, detach, or revoke operations retain the remaining ownership state for another `Deactivate()` call.

- **`source/swf_mime_filter.h/.cpp`** — Process-local URLMon handling for direct top-level SWF navigation:
  - `Initialize()` obtains the process `IInternetSession` and creates an in-memory `IClassFactory`. The factory is passed directly to URLMon; its CLSID is not registered with COM, and no protocol/MIME mapping is written to the registry.
  - `IsSupportedSwfUrl()` canonicalizes HTTP, HTTPS, `file:` URLs, DOS paths, and UNC paths with `IUri`. Every initially armed URL must have a `.swf` path suffix; a local target must also exist and must not be a directory.
  - `Arm()` installs a one-shot MIME filter for `application/x-shockwave-flash`. HTTP(S) responses with another MIME type retain IE's native behavior. Local files additionally use a temporary `file` namespace handler because URLMon's built-in file protocol bypasses MIME filters and classifies SWF data as `application/octet-stream`.
  - `ClaimPendingNavigation()` accepts an exact canonical URL match. HTTP(S) redirects may use the final URL only for a top-level binding with no root/document URL; local `file:` bindings never use this redirect fallback. The URLMon handlers are unregistered before the generated page's `<embed>` requests the actual SWF.
  - `SwfMimeProtocol` returns an in-memory UTF-8 HTML document containing a full-window Flash `<embed>`. It reports `text/html` to URLMon, serves bytes through `Read`, and defers `ReportResult` to a 1ms timer on the owning message-loop thread so URLMon cannot synchronously destroy the protocol from inside `Read`.
  - Pending handler registration, generation state, and the deferred completion object are protected by an SRW lock. `Cancel()` and `Shutdown()` unregister all temporary handlers; shutdown also cancels any pending completion timer.

- **`source/browser.h/.cpp`** — `BrowserHost` and OLE site classes (`COleSite`, `COleClientSite`, `COleInPlaceSite`, `COleInPlaceFrame`). Implements the standard OLE container interfaces needed to host an `IWebBrowser2` (IE) control in-process. `COleInPlaceFrame::SetActiveObject` retains the current `IOleInPlaceActiveObject` for accelerator and frame-activation forwarding. `BrowserHost` accepts keyboard messages only from the live browser window hierarchy, refreshes the browser child HWND when navigation replaces it, and also recognizes the container HWND used by windowless controls. `COleSite` implements:
  - `IDocHostUIHandler` — disables 3D border (`DOCHOSTUIFLAG_NO3DBORDER`).
  - `IOleCommandTarget` — suppresses script error dialogs (`OLECMDID_SHOWSCRIPTERROR`).
  - `IServiceProvider` / `IInternetSecurityManager` — returns `URLPOLICY_ALLOW` only for `URLACTION_HTML_MIXED_CONTENT`, so legacy HTTP subresources on HTTPS pages load without the IE mixed-content dialog. Every other URL security action uses IE's default policy, and no Internet Options registry setting is changed.
  - `IDispatch` — `DWebBrowserEvents2` event sink. `BeforeNavigate2` arms direct SWF handling only for the top-level browser identity. `NavigateComplete2`, `NavigateError`, and `FileDownload` clear unconsumed one-shot handlers. It also handles loading state, title/status changes, and NewWindow2/NewWindow3 (redirects new windows to the same browser).
  - `DISPID_AMBIENT_DLCONTROL` ambient property — allows content downloads but blocks ActiveX CAB downloads via `DLCTL_NO_DLACTIVEXCTLS`.
  - Back, Forward, Stop, and ordinary Navigate calls cancel pending SWF handling before starting another operation. Refresh re-arms a supported direct SWF URL before calling `IWebBrowser2::Refresh()`.
  - `Initialize()`/`Destroy()` own the lifetime of the process URLMon SWF manager in addition to the WebBrowser control and event connection.

- **`source/flash.h/.cpp`** — MIDL-generated Flash COM interface definitions (`IShockwaveFlash`, `CLSID_ShockwaveFlash`, `LIBID_ShockwaveFlashObjects`, etc.).

- **`source/debug.h/.cpp`** — `DbgTrace` macro for diagnostic output.

- **`source/text_encoding.h/.cpp`** — Shared strict UTF-16/UTF-8 conversion used by script preprocessing, SWC diagnostics, and generated SWF wrapper documents.

- **`source/resource.h`** / **`source/flashie.rc.in`** — Shared Win32 resource identifiers and the generated executable resources. The resource script embeds version metadata and the multi-size application icon.

- **`source/main.cpp`** — Win32 window with a toolbar (Back/Forward/Refresh/Stop/address bar/Go) and a browser area. The first command-line argument is used as the initial address; without one, the browser opens `https://www.bing.com/`. `WM_ACTIVATE` is forwarded to the current OLE active object, and the message loop offers browser keyboard messages to its `TranslateAccelerator` before normal dispatch. Initialization order: parse command line → `OleInitialize` → `FlashLoader::Activate()` → create window → `FlashLoader::InstallHooks()` (force-loads mshtml/urlmon/ieframe and atomically installs COM/registry/file/host-identity/WLDP/TypeLib hooks) → `BrowserHost::Initialize()` (initializes the URLMon SWF manager and creates the browser) → navigate. If hook installation fails, it warns the user and deactivates the loader before continuing without Flash support for that session. Shutdown destroys `BrowserHost` and its temporary URLMon registrations before `FlashLoader::Deactivate()` and `OleUninitialize()`.

- **`source/app.manifest`** — Registration-Free COM declarations for Flash.ocx (SxS activation context).

### Assets

- **`assets/flash/Flash32.ocx`** / **`assets/flash/Flash64.ocx`** — Pre-patched Flash Player ActiveX (32-bit / 64-bit).
- **`assets/flash/Flash32_Win7.ocx`** / **`assets/flash/Flash64_Win7.ocx`** — Flash Player ActiveX for Windows 7 and earlier (32-bit / 64-bit), selected with `-DFLASHIE_WINDOWS_TARGET=WIN7`. These controls have rendering problems on newer Windows versions.
- **`assets/icons/flashie.ico`** — Pixel-art application icon containing 16×16, 32×32, and 64×64 images.
- **`assets/icons/flashie-icon-16.svg`**, **`flashie-icon-32.svg`**, and **`flashie-icon-64.svg`** — Editable pixel-art source images for the corresponding icon sizes.

## Core Requirement: Zero Registry Pollution

The program MUST NOT register Flash.ocx into the system registry, and MUST NOT write to or modify the system registry. All emulated registration and policy effects must be strictly process-local; read-only opens of existing keys are allowed for valid handles and pass-through queries:

- **No `regsvr32` or `DllRegisterServer`**: Flash.ocx (alongside the exe) is loaded via `LoadLibrary` + `DllGetClassObject` only. The class factory is registered in-process via `CoRegisterClassObject`, never written to `HKCR` or `HKLM`.
- **No registry writes**: Registry hooks (`RegOpenKeyExW`, `RegQueryValueExW`) return fake in-memory responses for Flash CLSID lookups. No actual registry keys are created or modified. Fake `HKEY` handles are opened read-only on existing unrelated keys — used only as valid handle values, never written to.
- **Process-scoped hooks**: All Detours hooks (COM, registry, file, host identity, WLDP, TypeLib) and vtable patches operate only within the current process's address space. They are removed on shutdown via `FlashLoader::Deactivate()` (`DetourDetach` + vtable restoration).

When adding new features or modifying hooks, ensure this invariant is preserved. Any code path that could write to the registry or register COM objects system-wide is a bug.

## Key Design Details

- **Inline hooking**: Uses Microsoft Detours (`DetourAttach`/`DetourDetach`) to intercept API-level function calls. Not IAT patching — intercepts all callers including `GetProcAddress`-resolved calls. Required exports are resolved first, and the API hook set is installed or removed in a single transaction so callers never observe a deliberately partial configuration.
- **Vtable hooking**: `IOleObject::SetClientSite`, `IQuickActivate::QuickActivate`, and `IActiveScriptParse::ParseScriptText` are patched directly because Detours targets function prologues rather than vtable slots. Flash activation vtables and every tracked JScript vtable are restored during shutdown; failures are diagnosed instead of silently discarding hook state.
- **Fake registry keys**: Uses real `HKEY` handles opened read-only on existing keys and tracks them under an SRW lock, because Windows internals dereference `HKEY` as a pointer — invented sentinel values can cause access violations. The hooks emulate only exact Flash-related paths and the current executable's browser-emulation value; they do not modify the registry or broadly neutralize policy values.
- **Flash host identity**: Flash's ActiveX navigation code recognizes `iexplore.exe` as a browser host before it uses the document's successful `IBindHost` service. `GetModuleFileNameA/W` are therefore overlaid only for calls originating in the locally loaded `Flash.ocx`, only for `hModule == nullptr`, and only by replacing the executable basename while preserving the real directory and Win32 buffer contract.
- **`LoggingClassFactory`**: Wraps the real Flash class factory. On `CreateInstance`, installs three hooks on the new Flash object: (1) a Detours `QueryInterface` hook for `IObjectSafety` injection, (2) an `IOleObject::SetClientSite` vtable hook for forced activation, and (3) an `IQuickActivate::QuickActivate` vtable hook for iframe activation. Registered as the COM class object (not the raw factory) so the intercepted Flash creation paths use it. `allowScriptAccess` and other property-bag values remain controlled by the page.
- **Scoped security exceptions**: Registry hooks recognize exact Flash CLSID paths, validated Flash ProgID paths, and the current executable's browser-emulation value. WLDP hooks recognize only the Flash CLSID or locally loaded `Flash.ocx` file/image. Requests for unrelated controls, code images, and registry policy values are delegated unchanged to Windows.
- **Direct SWF navigation**: Temporary process-local URLMon handlers transform only the armed top-level SWF navigation while preserving its URL, history, and origin. Pending handlers are removed when the navigation ends or is canceled.
- **Synchronization and shutdown**: Shared hook and URLMon state use SRW locks. `BrowserHost::Destroy()` shuts down URLMon handling before `FlashLoader::Deactivate()` restores the remaining hooks and registrations.
- **Forced activation**: Windows 10 defers Flash `DoVerb(INPLACEACTIVATE)` until user click. Two vtable hooks solve this:
  - `SetClientSite` hook — catches the normal MSHTML activation path.
  - `QuickActivate` hook — catches the alternative path used by cross-domain iframes.
  - Both feed one object-deduplicated queue serviced by a 100ms coalescing timer. Only objects whose activation context is unavailable or whose `DoVerb` call fails enter the 200ms deferred queue (max 50 retries / 10 seconds); successful objects are removed immediately and are not activated again.

## Debugging

All diagnostic output uses `OutputDebugStringW` with `[FlashIE]` prefix. View with Visual Studio debugger Output window or Sysinternals DebugView.

## Git Commit Conventions

- Use Conventional Commits: `<type>(<scope>): <summary>`
- Example types: `feat`, `fix`, `refactor`, `build`, `docs`, `test`, `chore`
- Use an optional scope for the affected project, area, class, or file
- Write concise English summaries
- For non-trivial changes, include a body explaining the reason and implementation/fix approach
