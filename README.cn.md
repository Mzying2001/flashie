# FlashIE

[English](README.md)

一个独立运行的 Windows 应用程序，无需在系统中安装 Flash 即可运行 Adobe Flash 内容。FlashIE 内嵌 Internet Explorer WebBrowser 控件，通过 API 钩子加载本地附带的 Flash.ocx——不修改注册表、不修改系统文件、完全绿色便携。

## 特性

- **无需安装** — 本地附带 Flash.ocx，无需 `regsvr32`，无需系统级 Flash 安装
- **零系统污染** — 所有 API 钩子仅作用于当前进程，程序退出时自动移除；不写入注册表、不修改系统文件、不留下任何痕迹
- **自动激活** — 自动激活 Flash 内容，无需用户点击，包括 iframe 中嵌入的 Flash
- **本地数据隔离** — Flash 本地共享对象（LSO）和配置文件被重定向到程序旁的 `FlashData/` 目录，而非 `%APPDATA%`

## 工作原理

FlashIE 使用内联函数钩子（Detour）在进程级别拦截 Windows API 调用：

1. **COM 钩子** — 拦截 `CoGetClassObject`、`CoCreateInstance` 等函数，将 Flash CLSID 请求重定向到本地附带的 Flash.ocx
2. **注册表钩子** — 在内存中伪造 Flash 注册表项（CLSID、InprocServer32、TypeLib、ProgID、MIME 类型）——**不会向实际注册表写入任何内容**
3. **安全钩子** — 通过 `WldpIsClassInApprovedList` 和 `CoInternetIsFeatureEnabled` 批准 Flash，绕过 Windows 10+ 的限制
4. **文件系统钩子** — 将 `%APPDATA%\Macromedia\Flash Player` 和 `%APPDATA%\Adobe\Flash Player` 路径重定向到本地 `FlashData/` 目录
5. **激活钩子** — 钩子 `IOleObject::SetClientSite` 和 `IQuickActivate::QuickActivate` 虚表，通过合并定时器强制 Flash 就地激活

所有钩子在进程初始化后安装，在程序退出时干净移除。**不会对系统产生任何持久性更改。**

## 构建

### 环境要求

- CMake 3.20+
- 支持 C++17 的 MSVC
- Windows SDK

### 构建步骤

```bash
# 32 位
cmake -S . -B build -A Win32
cmake --build build --config Release

# 64 位
cmake -S . -B build -A x64
cmake --build build --config Release
```

构建过程会自动将对应架构的 `Flash.ocx` 和 `mms.cfg` 复制到输出目录的 `Flash/` 下。

## 使用方法

1. 构建或下载 `FlashIE.exe`
2. 确保 `Flash/` 目录（包含 `Flash.ocx` 和 `mms.cfg`）与可执行文件位于同一目录
3. 运行 `FlashIE.exe`
4. 在地址栏中输入包含 Flash 内容的 URL 或本地文件路径

Flash 数据（LSO 等）将存储在可执行文件旁的 `FlashData/` 目录中，而非系统的 `%APPDATA%`。

## 项目结构

```
flashie/
├── source/
│   ├── flash_loader.h/cpp    # API 钩子引擎（COM、注册表、安全、文件系统钩子）
│   ├── browser.h/cpp         # IE WebBrowser 控件的 OLE 容器
│   ├── flash.h/cpp           # MIDL 生成的 Flash COM 接口定义
│   ├── main.cpp              # Win32 窗口、工具栏和初始化
│   └── debug.h               # 调试输出宏
├── assets/
│   ├── Flash32.ocx           # 32 位 Flash Player ActiveX 控件
│   ├── Flash64.ocx           # 64 位 Flash Player ActiveX 控件
│   ├── mms.cfg               # Flash 配置（禁用生命周期终止检查和自动更新）
│   └── app.manifest          # 免注册 COM 清单
├── CMakeLists.txt            # 构建配置
└── AGENTS.md                 # 架构文档
```

## 许可证

MIT License
