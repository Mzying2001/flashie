# FlashIE

[English](README.md)

一个独立运行的 Windows 应用程序，无需在系统中安装 Flash 即可运行 Adobe Flash 内容。FlashIE 内嵌 Internet Explorer WebBrowser 控件，通过 API 钩子加载本地附带的 Flash.ocx——不修改注册表，完全绿色便携。

## 特性

- **无需安装** — 本地附带 Flash.ocx，无需 `regsvr32`，无需系统级 Flash 安装
- **零注册表污染** — 所有 API 钩子仅作用于当前进程，程序退出时自动移除；不修改注册表，也不进行系统级 COM 注册
- **自动激活** — 自动激活 Flash 内容，无需用户点击，包括 iframe 中嵌入的 Flash
- **直接导航到 SWF** — 在浏览器中直接打开顶层 HTTP(S) `.swf` URL，而不是下载文件，并保留原始 URL、历史记录项、重定向关系和文档源（origin）
- **本地 SWF 文件** — 打开通过 DOS 路径、UNC 路径或 `file:///` URL 指定的现有 `.swf` 文件，支持包含空格或 Unicode 字符的路径

## 工作原理

FlashIE 使用内联函数钩子（Detour）在进程级别拦截 Windows API 调用：

1. **COM 钩子** — 拦截 `CoGetClassObject`、`CoCreateInstance` 等函数，将 Flash CLSID 请求重定向到本地附带的 Flash.ocx
2. **注册表钩子** — 在内存中伪造 Flash 注册表项（CLSID、InprocServer32、TypeLib、ProgID、MIME 类型）——**不会向实际注册表写入任何内容**
3. **安全钩子** — 通过 `WldpIsClassInApprovedList` 和 `WldpQueryDynamicCodeTrust`，仅批准 Flash CLSID 和程序附带的 Flash.ocx 映像
4. **激活钩子** — 钩子 `IOleObject::SetClientSite` 和 `IQuickActivate::QuickActivate` 虚表，通过合并定时器强制 Flash 就地激活
5. **直接 SWF URLMon 处理** — 使用临时的进程内 URLMon 处理器，将顶层 HTTP(S) 和本地 SWF 导航显示为全窗口 Flash 内容，同时保持原始导航 URL 不变

API 钩子在进程初始化后安装，并在程序退出时移除。临时 URLMon 处理器会在一次导航被接管或放弃后立即移除，程序退出时还会执行最终清理。**不会对注册表产生任何持久性更改。**

## 构建

### 环境要求

- CMake 3.20+
- 支持 C++17 的 MSVC
- Windows SDK

### 构建步骤

```bash
# 32 位（Windows 8 及更高版本，默认配置）
cmake -S . -B build-x86 -A Win32
cmake --build build-x86 --config Release

# 64 位（Windows 8 及更高版本，默认配置）
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release

# 32 位 Windows 7 目标（使用兼容 Windows 7 及更早系统的 OCX）
cmake -S . -B build-win7-x86 -A Win32 -DFLASHIE_WINDOWS_TARGET=WIN7
cmake --build build-win7-x86 --config Release

# 64 位 Windows 7 目标（使用兼容 Windows 7 及更早系统的 OCX）
cmake -S . -B build-win7-x64 -A x64 -DFLASHIE_WINDOWS_TARGET=WIN7
cmake --build build-win7-x64 --config Release
```

`FLASHIE_WINDOWS_TARGET` 支持 `WIN7` 和 `WIN8`（默认值）。`WIN7` 会打包兼容 Windows 7 及更早系统的控件，`WIN8` 会打包适用于 Windows 8 及更新系统的控件。Win7 控件在新版 Windows 上存在渲染问题，请勿在新版系统中使用。构建过程还会设置相应的 Windows API 和 PE 子系统目标，并将选中的控件以 `Flash.ocx` 文件名复制到可执行文件同目录下。

## 使用方法

1. 构建或下载 `FlashIE.exe`
2. 确保 `Flash.ocx` 与可执行文件位于同一目录
3. 运行 `FlashIE.exe`
4. 在地址栏中输入包含 Flash 内容的 URL 或本地文件路径

若要在启动时直接加载地址，请将 URL 或本地文件路径作为第一个参数传入：

```powershell
FlashIE.exe "https://example.com/flash.html"
FlashIE.exe "https://example.com/movie.swf"
FlashIE.exe "C:\Games\Flash\movie.swf"
FlashIE.exe "file:///C:/Games/Flash/movie.swf"
```

直接本地导航支持以绝对 DOS 路径、UNC 路径或 `file:///` URL 指定的现有 `.swf` 文件。对于直接 HTTP(S) SWF 导航，初始请求的 URL 路径必须以 `.swf` 结尾，并且服务器最终响应的 MIME 类型必须为 `application/x-shockwave-flash`；支持 HTTP 重定向，而以其他下载 MIME 类型返回的响应仍采用 IE 原生下载行为。直接转换仅应用于顶层导航，不会覆盖网页对其嵌入 Flash 对象的配置。

## 项目结构

```
flashie/
├── source/
│   ├── flash_loader.h/cpp    # API 钩子引擎（COM、注册表、安全、TypeLib 钩子）
│   ├── swf_mime_filter.h/cpp # 直接处理 HTTP(S) 和本地 SWF 的 URLMon 逻辑
│   ├── browser.h/cpp         # IE WebBrowser 控件的 OLE 容器
│   ├── flash.h/cpp           # MIDL 生成的 Flash COM 接口定义
│   ├── main.cpp              # Win32 窗口、工具栏和初始化
│   ├── debug.h               # 调试输出宏
│   └── app.manifest          # 免注册 COM 清单
├── assets/
│   ├── Flash32.ocx           # 32 位 Flash Player ActiveX 控件
│   ├── Flash64.ocx           # 64 位 Flash Player ActiveX 控件
│   ├── Flash32_Win7.ocx      # 兼容 Windows 7 及更早系统的 32 位控件
│   ├── Flash64_Win7.ocx      # 兼容 Windows 7 及更早系统的 64 位控件
├── CMakeLists.txt            # 构建配置
└── AGENTS.md                 # 架构文档
```

## 许可证

MIT License
