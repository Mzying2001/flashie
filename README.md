# FlashIE

[中文版](README.cn.md)

A standalone Windows application that runs Adobe Flash content without requiring Flash to be installed on the system. FlashIE embeds an Internet Explorer WebBrowser control and loads a bundled Flash.ocx through API hooking — no registry modifications, no system file changes, completely portable.

## Features

- **No Installation Required** — Bundles Flash.ocx locally; no `regsvr32`, no system-wide Flash installation needed
- **Zero Registry Pollution** — All API hooks are process-scoped and removed on shutdown; no registry writes, no traces left behind
- **Auto-Activation** — Automatically activates Flash content without user clicks, including Flash embedded in iframes

## How It Works

FlashIE uses inline function hooking (detours) to intercept Windows API calls at the process level:

1. **COM Hooks** — Intercept `CoGetClassObject`, `CoCreateInstance`, etc. to redirect Flash CLSID requests to the bundled Flash.ocx
2. **Registry Hooks** — Fake Flash registry entries (CLSID, InprocServer32, TypeLib, ProgID, MIME types) entirely in memory — nothing is written to the actual registry
3. **Security Hooks** — Approve Flash through `WldpIsClassInApprovedList` and `CoInternetIsFeatureEnabled` to bypass Windows 10+ restrictions
4. **Activation Hooks** — Hook `IOleObject::SetClientSite` and `IQuickActivate::QuickActivate` vtables to force Flash in-place activation via a coalesced timer

All hooks are installed after process initialization and cleanly removed on shutdown. **No persistent changes are made to the system.**

## Build

### Requirements

- CMake 3.20+
- MSVC with C++17 support
- Windows SDK

### Build Steps

```bash
# 32-bit
cmake -S . -B build -A Win32
cmake --build build --config Release

# 64-bit
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build automatically copies the architecture-matched `Flash.ocx` to the output directory under `Flash/`.

## Usage

1. Build or download `FlashIE.exe`
2. Ensure the `Flash/` directory (containing `Flash.ocx`) is alongside the executable
3. Run `FlashIE.exe`
4. Enter a URL or local file path containing Flash content in the address bar

## Project Structure

```
flashie/
├── source/
│   ├── flash_loader.h/cpp    # API hooking engine (COM, registry, security, TypeLib hooks)
│   ├── browser.h/cpp         # OLE container for IE WebBrowser control
│   ├── flash.h/cpp           # MIDL-generated Flash COM interface definitions
│   ├── main.cpp              # Win32 window, toolbar, and initialization
│   └── debug.h               # Debug output macro
├── assets/
│   ├── Flash32.ocx           # 32-bit Flash Player ActiveX control
│   ├── Flash64.ocx           # 64-bit Flash Player ActiveX control
│   └── app.manifest          # Registration-free COM manifest
├── CMakeLists.txt            # Build configuration
└── AGENTS.md                 # Architecture documentation
```

## License

MIT License
