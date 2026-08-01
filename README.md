# ![icon](assets/icons/flashie-icon-32.svg) FlashIE

**English** | [中文](README.cn.md)

A standalone Windows application that runs Adobe Flash content without requiring Flash to be installed on the system. FlashIE embeds an Internet Explorer WebBrowser control and loads a bundled Flash.ocx through API hooking — no registry modifications, completely portable.

## Features

- **No Installation Required** — Bundles Flash.ocx locally; no `regsvr32`, no system-wide Flash installation needed
- **Zero Registry Pollution** — All API hooks are process-scoped and removed on shutdown; no registry or system-wide COM registration changes are made
- **Auto-Activation** — Automatically activates Flash content without user clicks, including Flash embedded in iframes
- **Windows 11 Rendering Compatibility** — Falls back from the broken SurfacePresenter path so windowless Flash draws, scrolls, and receives input at its actual page position
- **Direct SWF Navigation** — Opens top-level HTTP(S) `.swf` URLs and local `.swf` files from DOS, UNC, or `file:///` paths directly in the browser
- **Modern JavaScript Syntax Compatibility** — Expands JScript conditional compilation and transpiles modern syntax such as arrow functions, `let`/`const`, classes, and destructuring to ES5 for both JScript engines

## How It Works

FlashIE uses inline function hooking (detours) to intercept Windows API calls at the process level:

1. **COM Hooks** — Intercept `CoGetClassObject`, `CoCreateInstance`, etc. to redirect Flash CLSID requests to the bundled Flash.ocx
2. **Registry Hooks** — Fake Flash registry entries (CLSID, InprocServer32, TypeLib, ProgID, MIME types) entirely in memory — nothing is written to the actual registry
3. **Security Hooks** — Approve only the Flash CLSID and bundled Flash.ocx image through `WldpIsClassInApprovedList` and `WldpQueryDynamicCodeTrust`
4. **Activation Hooks** — Hook `IOleObject::SetClientSite` and `IQuickActivate::QuickActivate` vtables to force Flash in-place activation via a coalesced timer
5. **Windowless Rendering Hooks** — On affected systems, hide broken SurfacePresenter interfaces from Flash and normalize `IViewObject::Draw` to local control coordinates
6. **Script Hook** — Hooks `IActiveScriptParse::ParseScriptText` and preprocesses JavaScript code through `JScriptCC` and `swc-es5-c-api` in sequence
7. **Direct SWF URLMon Handling** — Uses temporary, process-local URLMon handlers to display top-level HTTP(S) and local SWF navigations as full-window Flash content without changing the original navigation URL

The SWC integration performs syntax transpilation only; it does not provide runtime polyfills such as `Promise` or `Symbol.iterator`, and it does not support JavaScript modules.

API hooks are installed after process initialization and removed on shutdown. Temporary URLMon handlers are removed as soon as their one navigation is claimed or abandoned, with shutdown cleanup as a final safeguard. **No persistent changes are made to the registry.**

## Build

### Requirements

- CMake 3.20+
- MSVC with C++17 support
- Windows SDK
- rustup (the pinned Rust nightly and `rust-src` component are selected by the `swc-es5-c-api` submodule)

### Build Steps

```bash
git submodule update --init --recursive

# 32-bit (Windows 8 or later, default)
cmake -S . -B build-x86 -A Win32
cmake --build build-x86 --config Release

# 64-bit (Windows 8 or later, default)
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release

# 32-bit Windows 7 target (uses the Windows 7-and-earlier OCX)
cmake -S . -B build-win7-x86 -A Win32 -DFLASHIE_WINDOWS_TARGET=WIN7
cmake --build build-win7-x86 --config Release

# 64-bit Windows 7 target (uses the Windows 7-and-earlier OCX)
cmake -S . -B build-win7-x64 -A x64 -DFLASHIE_WINDOWS_TARGET=WIN7
cmake --build build-win7-x64 --config Release
```

The FlashIE build invokes Cargo automatically, builds `swc-es5-c-api` for the matching Win7-baseline MSVC target, and links its C static API into `FlashIE.exe`. The first build may download the pinned Rust toolchain and locked Cargo dependencies.

`FLASHIE_WINDOWS_TARGET` accepts `WIN7` and `WIN8` (default). `WIN7` packages the control compatible with Windows 7 and earlier; `WIN8` packages the control for Windows 8 and later. The Win7 control automatically uses the legacy windowless-rendering fallback when run on newer Windows, while the default control uses it on Windows 11. The build also sets the corresponding Windows API and PE subsystem target, then copies the selected control to the output directory as `Flash.ocx`.

## Usage

1. Build or download `FlashIE.exe`
2. Ensure `Flash.ocx` is alongside the executable
3. Run `FlashIE.exe`
4. Enter a URL or local file path containing Flash content in the address bar

To load an address at startup, pass the URL or local file path as the first argument:

```powershell
FlashIE.exe "https://example.com/flash.html"
FlashIE.exe "https://example.com/movie.swf"
FlashIE.exe "C:\Games\Flash\movie.swf"
FlashIE.exe "file:///C:/Games/Flash/movie.swf"
```

Direct local navigation accepts existing `.swf` files specified as absolute DOS paths, UNC paths, or `file:///` URLs. For direct HTTP(S) SWF navigation, the initially requested URL path must end in `.swf` and the server's final response must return `application/x-shockwave-flash`; HTTP redirects remain supported, while responses advertised as unrelated download MIME types retain IE's native download behavior. Direct conversion applies only to the top-level navigation and does not override how web pages configure their embedded Flash objects.

## License

MIT License
