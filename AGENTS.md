# AGENTS.md

## Overview

FlashIE is a standalone Windows application that embeds IE's WebBrowser control and loads a bundled, pre-patched `Flash.ocx`. It supports Flash activation without user clicks, JavaScript preprocessing/transpilation, and full-window playback of direct SWF navigations.

## Core Requirement: Zero Registry Pollution

The application MUST NOT create, write, or modify system registry entries or register Flash system-wide. Keep all emulated registration and compatibility changes process-local.

- Load `Flash.ocx` beside the executable through `LoadLibrary` and `DllGetClassObject`; publish the wrapped factory with in-process `CoRegisterClassObject`. Never use `regsvr32` or `DllRegisterServer`.
- Emulate registry values in memory. Fake keys use real handles opened read-only on existing keys, tracked under an SRW lock and closed on normal close or shutdown. Invented `HKEY` values can crash Windows internals.
- Scope exceptions to the intended Flash class, local OCX, or current executable as described below. Preserve Windows behavior for unrelated requests.
- Keep API hooks and vtable patches within this process and remove them through `FlashLoader::Deactivate()`.

## Build

Use MSVC, CMake 3.20+, and C++17 with static CRT (`/MT` for Release, `/MTd` for Debug). Rust/Cargo must be available; `deps/swc-es5-c-api/rust-toolchain.toml` selects the pinned nightly and `rust-src`. CMake builds its architecture-matched Win7 MSVC target for both Windows profiles.

Run from the repository root:

```powershell
git submodule update --init --recursive

# Default profile: Windows 8 and later
cmake -S . -B build-x86 -A Win32
cmake -S . -B build-x64 -A x64

# Windows 7 profile
cmake -S . -B build-win7-x86 -A Win32 -DFLASHIE_WINDOWS_TARGET=WIN7
cmake -S . -B build-win7-x64 -A x64 -DFLASHIE_WINDOWS_TARGET=WIN7

# Use the configured build directory; replace Release with Debug as needed
cmake --build build-x86 --config Release
```

Output: `<build-directory>/<Config>/FlashIE.exe`. The post-build step copies the architecture- and profile-matched OCX beside it as `Flash.ocx`. `assets/flash/Flash32.ocx` and `Flash64.ocx` serve the default `WIN8` profile; their `_Win7.ocx` variants serve `WIN7`.

## Source Map

Paired headers and implementations are under `source/`.

| File | Responsibility |
| --- | --- |
| `flash_loader.h/.cpp` | Flash loading, COM factory, compatibility hooks, activation, rendering, and script processing. |
| `swf_mime_filter.h/.cpp` | Temporary URLMon handlers for direct top-level SWF playback. |
| `browser.h/.cpp` | `BrowserHost`, OLE container sites, browser events, navigation, and keyboard routing. |
| `main.cpp` | Win32 UI, command-line address, message loop, and startup/shutdown orchestration. |
| `flash.h/.cpp` | MIDL-generated Flash COM interfaces and GUIDs. |
| `text_encoding.h/.cpp` | Shared strict UTF-16/UTF-8 conversion. |
| `window_theme.h/.cpp`, `debug.h/.cpp` | Dark mode and diagnostic logging. |
| `resource.h`, `flashie.rc.in`, `app.manifest` | Resource IDs, version/icon resources, and application/SxS settings including registration-free Flash COM declarations. |

Dependencies are submodules under `deps/`, linked as static libraries: `Detours` for inline hooks, `JScriptCC` for Conditional Compilation, and Rust `swc-es5-c-api` for ES5 transpilation through a C ABI. Application icons and editable SVG sources are under `assets/icons/`.

## Lifecycle and Hook Ownership

Preserve startup order in `main.cpp`:

`OleInitialize` -> `FlashLoader::Activate()` -> create window -> `FlashLoader::InstallHooks()` -> `BrowserHost::Initialize()` -> navigate.

- `Activate()` and `InstallHooks()` are idempotent on the owning STA. Activation publishes the factory only after the OCX, wrapped factory, process-local COM registration, and rendering fallback decision are ready.
- `InstallHooks()` force-loads `mshtml`, `urlmon`, and `ieframe`, resolves required exports, then installs the API hook set in one Detours transaction. On failure, the UI warns and deactivates the loader before creating the browser.
- API hooks and Flash's `QueryInterface` use Detours, including for dynamically resolved calls. Flash activation/rendering, client-site `QueryInterface`, and script parsing use direct vtable patches. Track original pointers for restoration.
- `LoaderState` owns loader resources, hook records, fake keys, and activation queues. Separate SRW locks protect lifecycle, factory, registry, Flash hooks, and script hooks; URLMon has its own state lock.
- Shutdown order is `BrowserHost::Destroy()` (including URLMon shutdown) -> `FlashLoader::Deactivate()` -> `OleUninitialize()`, on the activation STA. Deactivation stops activation timers, restores tracked vtables, transactionally detaches API/Flash QI hooks, closes fake keys, revokes the COM class object, and releases factories and the loader's OCX reference. Restoration, detach, or revoke failures retain remaining ownership state for a later `Deactivate()` retry.

## Flash Compatibility Details

These constraints apply when changing `flash_loader.cpp`:

- **COM creation and safety:** COM hooks route Flash requests through `LoggingClassFactory`, which hooks each new object's QI, `SetClientSite`, `QuickActivate`, and `IViewObject` drawing methods. `FlashSafetyTearoff` supplies `IObjectSafety` with its own interlocked lifetime while delegating COM identity to Flash; MSHTML requires that shared `IUnknown` identity for scripting. Preserve Flash's `IPersistPropertyBag` behavior and page-controlled values such as `allowScriptAccess`. `LoadRegTypeLib` redirects the Flash TypeLib GUID to the local OCX via `LoadTypeLibEx`.
- **Registry emulation:** `RegOpenKeyExW`, `RegQueryValueExW`, and `RegCloseKey` emulate Flash registration, MIME/version detection, kill-bit bypass, and the current executable's `FEATURE_BROWSER_EMULATION` value. Match complete CLSID/validated ProgID path elements case-insensitively, preserve Win32 query buffer contracts, and leave unrelated `Compatibility Flags` values unchanged.
- **WLDP:** Approve only the Flash CLSID and trust only the loaded local OCX file/image. Install the two WLDP hooks only when both exports exist; older Windows may provide neither.
- **Forced activation:** `SetClientSite` and `QuickActivate` feed one object-deduplicated queue on the activation STA. A 100ms timer calls `DoVerb(OLEIVERB_INPLACEACTIVATE)` only with a valid site, parent HWND, and position rectangle. Only unavailable activation context or failed `DoVerb` enters the 200ms retry queue, bounded to 50 timer retries (nominally 10 seconds). Remove successful objects immediately to avoid repeated activation.
- **Windowless rendering:** On Windows 11 and later, or when the Win7 OCX runs on a newer OS, hide MSHTML SurfacePresenter interfaces only from local Flash callers through the client-site QI hook. This avoids lost DOM offsets; `IViewObject::Draw` normalizes the HDC and bounds to local control coordinates.
- **Browser identity:** `GetModuleFileNameA/W` substitutes only the executable basename with `iexplore.exe`, only for local Flash callers querying `hModule == nullptr`. This enables Flash's `navigateToURL` browser path. Preserve the real directory and Win32 buffer contract.
- **SharedObject first save:** `DeleteFileA/W` treats `ERROR_FILE_NOT_FOUND` as success only for local Flash callers deleting a `.sol`, allowing Flash's own `.sxx` commit to continue.
- **Script processing:** Hook classic JScript and JScript9 only, using the SDK-selected `IID_IActiveScriptParse` for the architecture. Convert UTF-16 to UTF-8 once, run JScriptCC, then SWC for complete scripts, and convert back. `SCRIPTTEXT_ISEXPRESSION` bypasses SWC. JScriptCC or encoding failure uses the original script; SWC failure keeps the preprocessed code. Track and restore every hooked engine vtable.

## Direct SWF Navigation

When changing `swf_mime_filter.cpp` or its browser event integration:

- Arm only top-level navigations with a `.swf` path suffix. `IUri` canonicalizes HTTP(S), `file:` URLs, DOS paths, and UNC paths; local targets must exist and be files.
- Pass an in-memory factory directly to the process `IInternetSession`. Use a one-shot `application/x-shockwave-flash` MIME filter; HTTP(S) responses of other types keep native IE behavior. Local files also need a temporary `file` namespace handler because the built-in file protocol bypasses MIME filters. The handler CLSID has no COM registration.
- Claim exact canonical URL matches. HTTP(S) redirect fallback accepts the final URL only when the binding has no root/document URL; local bindings require an exact match. Unregister handlers before the wrapper's `<embed>` loads the SWF. Serve an in-memory UTF-8 `text/html` wrapper at the navigation URL, preserving browser history and origin.
- Defer `ReportResult` through a timer on the owning message-loop thread; calling it synchronously inside `Read` can destroy the protocol during the call. `Cancel()`/`Shutdown()` remove temporary handlers; shutdown also cancels deferred completion.
- `BeforeNavigate2` arms handling only for the top-level browser COM identity. `NavigateComplete2`, `NavigateError`, and `FileDownload` clear unconsumed handlers. Navigate/Back/Forward/Stop cancel pending handling; Refresh re-arms supported SWF URLs.

## Browser Host Behavior

- The OLE sites host `IWebBrowser2`, suppress 3D borders and script-error dialogs, block ActiveX CAB downloads via `DLCTL_NO_DLACTIVEXCTLS`, and redirect new windows into the current browser.
- `IInternetSecurityManager` allows only `URLACTION_HTML_MIXED_CONTENT` explicitly; all other actions use IE's default policy. Keep Internet Options unchanged.
- Forward `WM_ACTIVATE` and browser keyboard messages to the current `IOleInPlaceActiveObject`. Accept input only from the live browser window hierarchy or the container HWND used by windowless controls; refresh the child HWND when navigation replaces it.

## Debugging and Commits

Debug builds emit `DbgTrace` messages through `OutputDebugStringW` with the `[FlashIE]` prefix; inspect them in Visual Studio's Output window or Sysinternals DebugView. Release builds compile out `DbgTrace`.

Use Conventional Commits: `<type>(<scope>): <summary>` (scope optional). Write concise English summaries; include a body explaining the reason and approach for non-trivial changes.
