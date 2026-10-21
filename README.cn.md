# ![icon](assets/icons/flashie-icon-32.svg) FlashIE

[English](README.md) | **中文**

一个独立运行的 Windows 应用程序，无需系统级安装 Flash 即可运行 Adobe Flash 内容。FlashIE 内嵌 Internet Explorer 的 WebBrowser 控件，通过进程内钩子加载随程序附带的 `Flash.ocx`。

## 特性

- **便携运行** — 随程序附带 `Flash.ocx`，无需 `regsvr32` 或系统级 Flash 安装
- **进程内运行** — 钩子仅作用于当前进程，并在退出时移除；不写入注册表或进行系统级 COM 注册
- **自动激活** — 自动激活 Flash 内容，无需用户点击，包括 iframe 中嵌入的 Flash
- **Windows 11 渲染兼容** — 避开有问题的 SurfacePresenter 路径，使无窗口 Flash 在页面实际位置绘制、滚动并接收输入
- **直接导航到 SWF** — 在浏览器中直接打开顶层 HTTP(S) `.swf` URL 和通过 DOS、UNC 或 `file:///` 路径指定的本地 `.swf` 文件
- **现代 JavaScript 语法兼容** — 展开 JScript 条件编译，并为两个 JScript 引擎将箭头函数、`let`/`const`、类、解构等现代语法转译为 ES5

## 工作原理

FlashIE 使用进程内 API 和虚表钩子来：

1. 将 Flash COM 请求转到本地控件，并在内存中提供所需的注册表项。
2. 通过限定范围的安全钩子放行 Flash CLSID 和随程序附带的控件映像。
3. 自动激活 Flash，并在受影响的系统上启用无窗口渲染回退。
4. 使用 `JScriptCC` 预处理 JScript，再通过 `swc-es5-c-api` 转译为 ES5。
5. 通过临时 URLMon 处理器显示顶层 SWF 导航，同时保持原始 URL 不变。

`swc-es5-c-api` 只转译语法，不提供 `Promise`、`Symbol.iterator` 等运行时 polyfill，也不支持 JavaScript 模块。程序退出时会清理钩子和临时 URLMon 处理器。

## 构建

### 环境要求

- CMake 3.20+
- 支持 C++17 的 MSVC
- Windows SDK
- rustup（`swc-es5-c-api` 子模块会选择固定的 Rust nightly 工具链及 `rust-src` 组件）

### 构建步骤

```bash
git submodule update --init --recursive

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

构建过程会自动调用 Cargo，为匹配的 Win7 基线 MSVC 目标编译 `swc-es5-c-api` 并链接到 `FlashIE.exe`。首次构建可能会下载固定版本的 Rust 工具链和依赖。

`FLASHIE_WINDOWS_TARGET` 支持 `WIN7` 和 `WIN8`（默认值），用于选择对应控件及 Windows API/PE 子系统目标。构建会将控件以 `Flash.ocx` 文件名复制到 `FlashIE.exe` 同目录。Win7 控件在较新 Windows 上使用旧式无窗口渲染回退；默认控件则在 Windows 11 上使用该回退。

## 使用方法

1. 构建或下载 `FlashIE.exe`，并将对应的 `Flash.ocx` 放在同一目录
2. 运行程序，在地址栏输入包含 Flash 内容的 URL 或本地文件路径

若要在启动时直接加载地址，请将 URL 或本地文件路径作为第一个参数传入：

```powershell
FlashIE.exe "https://example.com/flash.html"
FlashIE.exe "https://example.com/movie.swf"
FlashIE.exe "C:\Games\Flash\movie.swf"
FlashIE.exe "file:///C:/Games/Flash/movie.swf"
```

本地导航支持通过绝对 DOS 路径、UNC 路径或 `file:///` URL 指定的现有 `.swf` 文件。HTTP(S) 导航要求请求路径以 `.swf` 结尾，且最终响应的 MIME 类型为 `application/x-shockwave-flash`。支持重定向；其他下载 MIME 类型仍采用 IE 原生行为。转换仅适用于顶层导航，不会改变网页对嵌入式 Flash 对象的配置。

## 许可证

MIT License
