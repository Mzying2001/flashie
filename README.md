# ![icon](assets/icons/flashie-icon-32.svg) FlashIE

**English** | [中文](README.cn.md)

A standalone Windows application for running Adobe Flash content without a system-wide Flash installation. FlashIE embeds Internet Explorer's WebBrowser control and loads a bundled `Flash.ocx` through process-local hooks.

## Features

- **Portable** — Bundles `Flash.ocx`; no `regsvr32` or system-wide Flash installation required
- **Process-Local** — Hooks are removed on shutdown; no registry writes or system-wide COM registration
- **Auto-Activation** — Automatically activates Flash content without user clicks, including Flash embedded in iframes
- **Windows 11 Rendering Compatibility** — Falls back from the broken SurfacePresenter path so windowless Flash draws, scrolls, and receives input at its actual page position
- **Direct SWF Navigation** — Opens top-level HTTP(S) `.swf` URLs and local `.swf` files from DOS, UNC, or `file:///` paths directly in the browser
- **Modern JavaScript Syntax Compatibility** — Expands JScript conditional compilation and transpiles modern syntax such as arrow functions, `let`/`const`, classes, and destructuring to ES5 for both JScript engines

## How It Works

FlashIE uses process-local API and vtable hooks to:

1. Redirect Flash COM requests to the bundled control and provide its expected registry values in memory.
2. Permit the bundled Flash CLSID and image through scoped security hooks.
3. Activate Flash automatically and apply a windowless-rendering fallback on affected systems.
4. Preprocess JScript with `JScriptCC` and transpile it to ES5 with `swc-es5-c-api`.
5. Use temporary URLMon handlers to display direct top-level SWF navigations without changing their URLs.

`swc-es5-c-api` transpiles syntax only. It does not provide runtime polyfills such as `Promise` or `Symbol.iterator`, and does not support JavaScript modules. Hooks and temporary URLMon handlers are cleaned up during shutdown.

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

The build invokes Cargo to compile `swc-es5-c-api` for the matching Win7-baseline MSVC target and links it into `FlashIE.exe`. The first build may download the pinned Rust toolchain and dependencies.

`FLASHIE_WINDOWS_TARGET` accepts `WIN7` or `WIN8` (default). It selects the matching OCX and Windows API/PE subsystem target; the build copies the OCX beside `FlashIE.exe` as `Flash.ocx`. The Win7 OCX uses the legacy windowless-rendering fallback on newer Windows; the default OCX uses it on Windows 11.

## Usage

1. Build or download `FlashIE.exe` with its matching `Flash.ocx` alongside it
2. Run `FlashIE.exe` and enter a URL or local file path containing Flash content

To load an address at startup, pass the URL or local file path as the first argument:

```powershell
FlashIE.exe "https://example.com/flash.html"
FlashIE.exe "https://example.com/movie.swf"
FlashIE.exe "C:\Games\Flash\movie.swf"
FlashIE.exe "file:///C:/Games/Flash/movie.swf"
```

Direct local navigation accepts existing `.swf` files specified by absolute DOS paths, UNC paths, or `file:///` URLs. For HTTP(S), the requested path must end in `.swf` and the final response must use the `application/x-shockwave-flash` MIME type. Redirects are supported; other download MIME types retain IE's native behavior. Conversion applies only to top-level navigation and does not change how pages configure embedded Flash objects.

## License

MIT License
