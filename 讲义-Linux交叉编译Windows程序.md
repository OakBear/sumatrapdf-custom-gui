# 从 Linux 到单个 EXE：一次 Windows 程序的交叉编译讲义

> 读者：项目发起人。目标是把这一路做过的事讲成可以复用的知识，而不是一次性的操作记录。
> 文中区分三类信息：**观察**（有命令输出/文件/日志为证）、**推导**（由观察按代码逻辑推出）、**未知**（尚未证实，不假装知道）。

## 0. 一页速览

| 问题 | 简短回答 |
| --- | --- |
| 我们做的是标准交叉编译吗？ | 是，但跨的是**操作系统**而不是 CPU 架构：Linux x86-64 主机 → Windows x86-64 目标。 |
| 为什么最后变成"Windows SDK"？ | 编译器只负责生成代码；C++ 运行时、Windows 头文件与导入库只能来自微软的 CRT/SDK。 |
| 第一次为什么没有界面？ | 程序默认走 Direct2D 绘制路径；该路径在 Wine 和你的 Windows 11 上都崩了。换成项目自带的 GDI+ 后端即可用。 |
| 以后改前端/后端会出错吗？ | 不会因为"交叉编译"这个身份出错；真正的坑是构建图、覆盖层、静态 CRT 和只在 Wine 里验证。 |
| Wine 是模拟器吗？ | 不是。它是 Windows API 的 Linux 重实现 + PE 加载器，Windows 机器码直接在 CPU 上跑。 |

产物事实（可核对）：`SumatraPDF-static.exe` 21,793,280 字节，PE 头 `Machine: AMD64`，导入表只有系统 DLL（无 `VCRUNTIME140.dll`、无 `mupdf.dll`）。最新校验值见 `SUMATRA-改造总览.md`（同源码重建时 PE 头 `TimeDateStamp` 会变，哈希不保证一致）。

## 1. 时间线：这件事是怎么一步步发生的

| 阶段 | 动作 | 支撑证据 | 结果 |
| --- | --- | --- | --- |
| 1 | 读 SumatraPDF 源码，确认开源可改 | 上游仓库、`git log` | 确定以它为基础 |
| 2 | 发现没有 Visual Studio，磁盘也不够 | 你的约束描述 | 放弃"装 VS 直接编" |
| 3 | 想到在 Linux 上产出 Windows EXE | 你的思路 | 目标定为跨 OS 交叉编译 |
| 4 | 备齐 LLVM 19 + 微软 CRT/SDK（xwin 下载） | `sumatrapdf-toolchain/TOOLCHAIN-INFO.md` | 编译器就位 |
| 5 | 用 Wine 跑上游 `premake5.exe` 生成构建图 | Wine 6.0.2 执行记录 | 得到 Ninja 图 |
| 6 | 写 `adapt-ninja.py` 修路径、补库名大小写 | 构建图与脚本 | Ninja 可执行 |
| 7 | 第一次链接成功 | 21 MB EXE | "编出来了" |
| 8 | Wine 里运行：窗口有、页面识别了、随后访问违例、截图全黑 | 0xc0000005 minidump、黑截图 | 第一次图形失败 |
| 9 | 关掉 Direct2D（`WINEDLLOVERRIDES=…d2d1=`）后能显示 | 非黑截图 OCR 出标题 | 锁定图形后端 |
| 10 | 把默认后端改为 GDI+ 并重新编译 | `local/Gfx.patch`（一行） | 兼容版可用 |
| 11 | 你在 Windows 11 上用上了 GDI+ 版 | 你的反馈 | 目标达成 |
| 12 | 追加会话三件套（快照/时间戳/多窗口） | Wine 自动化测试 | 功能完成 |
| 13 | 缓存上限对照实验 | RSS 表格 | **负结果**：无收益 |
| 14 | 拆解 PDF-XChange 11 看性能设计 | 解包 1.1 GB、导入表、插件清单 | 得到可借鉴清单 |

## 2. 什么是交叉编译（问题 1 的正面回答）

### 2.1 交叉编译有两个轴，不是一个

很多人一听"交叉编译"就想到"给 ARM 编程序"。其实编译器有**两个独立的目标维度**：

- **架构轴**：x86-64 / ARM64 / RISC-V …
- **系统轴**：Linux / Windows / macOS / 裸机 …

两个轴可以任意组合。我们的情况是：

| | 主机（Host，跑编译器的机器） | 目标（Target，跑产物的机器） |
| --- | --- | --- |
| CPU 架构 | x86-64 | x86-64（**相同**） |
| 操作系统 | Linux | Windows（**不同**） |
| 可执行格式 | ELF | PE/COFF（**不同**） |
| C++ ABI | Itanium/GCC | MSVC（**不同**） |

四个维度里三个不同，所以这**是**标准的交叉编译，只是跨的是系统和 ABI，而不是 CPU。
官方术语里有时叫 **cross-OS build** 或 **cross-target build**，以区别于 cross-architecture。

### 2.2 为什么你"没有体会到"

因为你没有换电脑，也没有换 CPU；而且编译命令看起来就是普通的 `clang-cl /c file.cpp`。
但只要看一眼产物就知道它不可能在 Linux 上直接跑：

- 文件头是 `MZ`/`PE`，不是 `ELF`；
- 导入表写的是 `KERNEL32.dll`、`USER32.dll`，不是 `libc.so`；
- 异常、名字修饰、结构体布局按 MSVC 规则；
- 启动代码是 `mainCRTStartup`，不是 `_start`。

### 2.3 "变成 Windows SDK"的本质

一个 C++ 程序从源码到 EXE，需要四类东西：

| 需要的东西 | 由谁提供 | 我们的来源 |
| --- | --- | --- |
| 编译器前端 + 代码生成 | Clang | LLVM 19.1.7（conda-forge / 清华镜像） |
| 标准库与运行时（`std::string`、`new`、异常、启动代码） | 微软 CRT | 14.44.17.14，xwin 从微软清单下载 |
| Windows API 的声明与导入库（`CreateWindow` 等） | 微软 Windows SDK | 10.0.26100，同上 |
| 链接器/归档器/资源编译器 | LLD 工具集 | `lld-link`、`llvm-lib`、`llvm-rc` |
| 构建系统 | 上游 Premake + Ninja | Premake 是 Windows 程序，靠 Wine 跑 |

关键认知：**`clang-cl` 只是"能说 MSVC 方言"的编译器**，它不附带任何微软的库和头文件。
所以"交叉编译到 Windows"在工程上就等于"拿 Linux 上的编译器 + 微软的 SDK/CRT 拼出一条工具链"。
这不是我们发明的偏方：Chromium、Rust 的 `xwin`、以及大量 CI 都这么干。

`xwin` 在这里的角色很朴素：它按微软 Visual Studio 的发布清单把 CRT/SDK 的包下载并解包到本地，
省去装 Visual Studio。它不参与编译，也不模拟 Windows。

### 2.4 为什么不能用 gcc / MinGW

- MinGW 用的是 **GCC 的 Windows ABI**（异常、名字修饰、部分结构体布局）与 MSVC 不同；
- 本项目按 MSVC 配置写（`/MT`、`#pragma`、MSVC 扩展、`__declspec` 语义、PDB 等）；
- 上游的第三方静态库也按 MSVC 目标配置；混用 ABI 会在链接或运行时炸。

`clang-cl` 的价值就在这：Clang 的代码生成 + MSVC 的 ABI 与命令行接口。

## 3. 构建流水线逐步拆解

```text
源码 (tree/src ← 上游 + local/ 覆盖)
        │
        ├─(1) premake5.exe  ← Wine 执行，读 premake5.lua
        │        └→ tree/.work/ninja/*.ninja（Windows 路径、MSVC 工具名）
        │
        ├─(2) adapt-ninja.py  ← 纯 Python，改写构建图
        │        ├→ 路径重定向（Gfx.cpp、ReadAloud_win.cpp 走 local/）
        │        ├→ 丢弃不可用目标（installer、ARM64 等）
        │        └→ 造大小写敏感库别名（Rstrtmgr/Bcrypt/PowrProf）
        │
        ├─(3) ninja -j4  ← Linux 原生
        │        ├→ clang-cl /c ...        （编译，MSVC ABI）
        │        ├→ llvm-lib              （静态库）
        │        ├→ MakeLZSA.exe ← Wine    （打包字体/翻译/JS 成 PAK）
        │        └→ lld-link /MT ...       （链接成单个 EXE）
        │
        └─(4) out/rel64/SumatraPDF-static.exe
```

每一步的"为什么"：

| 步骤 | 为什么需要它 | 能不能去掉 |
| --- | --- | --- |
| Premake 在 Wine 里跑 | 上游只提供 Windows 版 `premake5.exe` | 可以，但要自己写等价的构建描述 |
| `adapt-ninja.py` | 生成的图里有 Windows 路径分隔符、`cl.exe`、绝对路径、不可用目标 | 可以，但等于手写构建图 |
| `MakeLZSA.exe`（Wine） | 上游用它把字体/翻译/JS 压成内嵌 PAK | 可以，用 Python 重写压缩器 |
| 静态 `/MT` | 产物要单文件、不依赖 VC 运行库 | 可以改成 `/MD`，但要多带 DLL |

## 4. 案例复盘：第一次为什么没有图形界面

### 4.1 现场证据（分环境）

| 环境 | 现象 |
| --- | --- |
| Linux + Wine 6.0.2 + Xvfb | 窗口能创建、能识别"两页"，随后访问违例（0xc0000005）并写 minidump，截图全黑 |
| 同一环境，禁用 `d2d1` | 不再新崩溃，截图非黑，OCR 能认出页面标题片段 |
| 你的 Windows 11 | 双击后只出现设置文件，界面没起来 |

### 4.2 代码里图形后端是怎么选的

`src/gui/Gfx.cpp` 里有一个全局开关：

```cpp
bool gUseDirect2D = true;      // 上游默认：优先 Direct2D

Gfx* GfxCreate(HDC hdc) {
    if (gUseDirect2D && Direct2DAvailable()) {
        auto* d2d = new GfxDirect2D(hdc);
        if (d2d->target) return d2d;   // 只有这里失败才回退
        delete d2d;
    }
    return new GfxGdiplus(hdc);       // GDI+ 后备
}
```

看起来"有回退"，为什么还会死？

### 4.3 回退为什么没救回来（推导）

1. 回退只覆盖 `GfxCreate` 这一条路。文字测量（`GfxMeasureText`）、部分图像/文本布局路径会**直接**调用 Direct2D/DirectWrite 的函数。
2. 更关键的是：如果崩溃发生在 `d2d1.dll` **内部**（访问违例），进程当场结束，回退代码根本没有机会执行。
3. `Direct2DAvailable()` 只是检查工厂对象能否创建；工厂建得出来 ≠ 每个渲染目标/文本布局都能用。

所以"有 fallback"和"能容错"是两件事。

### 4.4 修复动作

把默认值改成 `false`（`local/Gfx.patch`，就是一行）：

```diff
-bool gUseDirect2D = true;
+bool gUseDirect2D = false;
```

重新编译后，Wine 下正常显示两页 PDF，命令行渲染和无效页码负例也通过。
注意：**这不是"修好了 Direct2D"**，而是"绕开了 Direct2D"。原路径的缺陷仍然存在。

### 4.5 Windows 11 侧到底是不是同一个原因？（未知，附取证方法）

已证实的是：Wine 里的崩溃与 Direct2D 路径相关。**未证实**的是：你 Windows 11 上那次失败与它是同一个根因。
两种可能都成立：

- (a) 我们的构建在这台机器上触发了 Direct2D/DirectWrite 路径的缺陷（显卡驱动、D3D 互操作、字体回退都可能参与）；
- (b) 失败发生在更早的窗口初始化，与图形无关（例如某个初始化步骤在真机上失败），恰好被 GDI+ 版"顺带"改变了时序。

要在 Windows 上分辨，可按顺序取三样证据：

1. **运行日志**：`SumatraPDF-static.exe -log-to-file Z:\temp\sp.log 你的文件.pdf`
   代码里 Direct2D 初始化会打印 `InitDirect2D: d2d1.dll=… dwrite.dll=…`、`D2D1CreateFactory failed 0x…`。
   如果日志里出现工厂失败或根本没到这一步，结论完全不同。
2. **崩溃转储**：`%LOCALAPPDATA%\SumatraPDF-data\<exe 的 SHA1 前 6 位>\crashinfo\*.dmp`
   （程序自带的崩溃处理器会写 `sumatrapdfcrash.dmp`；文件名带进程号时是 `sumatrapdfcrash-<pid>.dmp`。）
3. **系统级记录**：事件查看器 → Windows 错误报告，或 `%LOCALAPPDATA%\Microsoft\Windows\WER\ReportArchive`。

拿到 .dmp 后用 WinDbg/`cdb` 看崩溃模块：若是 `d2d1.dll`/`dwrite.dll`，基本可确认是图形路径；若是别的模块，说明还有第二个问题。

### 4.6 正确的结论表述

| 说法 | 是否成立 |
| --- | --- |
| "第一次编译错了" | 不成立。编译、链接、PE 结构都正确，程序能启动到创建设置文件。 |
| "第一次的程序在图形初始化/绘制阶段崩溃" | 观察成立（Wine 有 minidump；Windows 侧只知未出界面）。 |
| "Direct2D 路径是根因" | 在 Wine 上成立；在 Windows 11 上是**最可疑的假设**，未被证实。 |
| "换成 GDI+ 就好了" | 观察成立：两个环境都能用。 |

### 4.7 一条通用的验证阶梯

| 层级 | 问题 | 我们的手段 |
| --- | --- | --- |
| L1 | 编过了吗 | Ninja 退出码 0、产物存在 |
| L2 | 是合法 Windows 程序吗 | `llvm-readobj` 看 Machine/Subsystem/导入表 |
| L3 | 跑得起来吗 | Wine 里启动、看进程与窗口 |
| L4 | 画出来了吗 | Xvfb 截图 + OCR / 非黑像素比例 |
| L5 | 画对了吗、交互对吗 | 人工目视 / 自动化点击 / 真机复测 |

第一次失败卡在 L4，而不是 L1。这两者的含义完全不同。

## 5. 以后要改前端/后端，会不会出错（问题 2）

### 5.1 先认清三层结构

| 层 | 典型文件 | 改它的风险 |
| --- | --- | --- |
| 界面/交互（前端） | `src/gui/*`、`src/SumatraPDF.cpp`、各种 `*Dialog.cpp`、`Toolbar.cpp` | 低。纯 Win32 + 项目自己的控件库，交叉编译没有额外门槛。 |
| 渲染/布局 | `src/gui/Gfx*.cpp`、`src/Canvas.cpp`、`RenderCache.cpp` | 中。涉及 Direct2D/GDI+ 两个后端时必须两边都验证。 |
| 引擎/格式（后端） | `src/shared/Engine*.cpp`、`src/shared/*Doc.cpp`、`ext/` 第三方库 | 中高。MuPDF/第三方库的行为只能在运行时验证；改 `ext/` 会触发全量重编。 |

### 5.2 改动的正确姿势

不要直接改 `tree/src/…`（它是覆盖层，且可能被重建覆盖），按这个循环：

```sh
cd /cluster/home/it_stu171/rea/sumatrapdf-build

# 1) 从上游原文生成本地副本（首次）
patch --batch -o local/Xxx.cpp ../sumatrapdf/src/Xxx.cpp < local/Xxx.patch

# 2) 编辑 local/Xxx.cpp

# 3) 让构建吃到它：
#    - 已在 adapt-ninja.py 里重定向的文件，直接生效；
#    - 新文件需要把路径指向 local/，并重新生成构建图

# 4) 增量构建（只重编受影响文件 + 链接）
bash build-static.sh

# 5) 冒烟：命令行渲染
WINEPREFIX=/tmp/spw-pfx wine tree/out/rel64/SumatraPDF-static.exe -render -zoom 1 Z:\\tmp\\t1.pdf

# 6) 取回并更新补丁
diff -u ../sumatrapdf/src/Xxx.cpp local/Xxx.cpp > local/Xxx.patch
```

判断一个文件是否"已被覆盖到 local/"：看 `adapt-ninja.py` 里有没有它的重定向，或直接看构建图里该目标的输入路径。

### 5.3 会让构建失败的五个坑

| 坑 | 表现 | 处理 |
| --- | --- | --- |
| 改了 `premake5.lua` 或增删源文件 | Ninja 不知道新文件 | 在 `tree/` 里重跑 Premake，再跑 `adapt-ninja.py` |
| 目录被移动/复制 | 图里全是绝对路径，链接报找不到文件 | 重建符号链接与工具链相对布局，重跑 Premake |
| 库名大小写 | `Rstrtmgr.lib` vs `rstrtmgr.lib` | 沿用 `build-static.sh` 里的别名目录 |
| 新增第三方依赖 | 缺头文件/库 | 用 xwin 的 SDK 或自行编成 `.lib`，注意 `/MT` 一致性 |
| 只在 Wine 验证 | 真机行为不同 | 关键改动必须在 Windows 上再跑一遍 |

### 5.4 调试手段清单

| 需求 | 做法 |
| --- | --- |
| 看程序内部日志 | `-log-to-file <path>` |
| 不开窗口验证渲染 | `-render -zoom 1 <pdf>` 输出 PNG |
| 无头 GUI 测试 | `Xvfb :97` + `wine` + `xdotool` 截图/发按键 |
| 排除用户配置干扰 | `-appdata <独立目录>` |
| 关掉某个系统 DLL | `WINEDLLOVERRIDES="d2d1="`（仅用于定位） |
| 看二进制结构 | `llvm-readobj --coff-imports/--file-headers` |

### 5.5 一个真实的改造例子（如果你想继续做内存）

目标：把"按张数上限"改成"全局字节预算 + 空闲超时"。涉及：

1. `src/shared/RenderCache.cpp`：记录每张位图的字节数、最后可见时间；淘汰时先看预算再看时间；
2. `src/Settings.cpp`：新增设置项（并用 `Settings.cpp` 的字段表注册）；
3. `src/shared/DisplayModel.cpp`：切换标签页时更新"最后可见"时间；
4. 验证：同一组 PDF，测量"打开 N 个文档 + 切换"的 RSS 曲线，与基线对比。

这条改动的风险点是**不能为了省内存销毁未保存的注释**，也不能让活动标签页的位图被误逐。

## 6. Wine 到底是什么（问题 3）

### 6.1 一句话定义

Wine = **Windows API 的重新实现** + **PE 可执行文件加载器**。
Windows 程序的机器码由本机 CPU **直接执行**，指令不翻译；被替换的是"程序调用的那些 Windows DLL"。

### 6.2 它替我们做了什么

| Windows 组件 | Wine 提供 |
| --- | --- |
| `kernel32.dll`、`user32.dll`、`gdi32.dll` | 开源实现，把调用翻译成 Linux 系统调用 |
| `d2d1.dll`、`dwrite.dll` | 部分实现（可能借助 OpenGL/Vulkan），**完整度不如真 Windows** |
| 注册表 | 存在 `~/.wine` 或 `WINEPREFIX` 里的文件 |
| 窗口/消息循环 | 接到 X11/Wayland |
| PE 加载、重定位、导入解析 | Wine 自己的 loader |

### 6.3 我们用它做了三件事

| 用途 | 为什么必须 | 说明 |
| --- | --- | --- |
| 跑 `premake5.exe` | 上游只给 Windows 版生成器 | 只需要它写出 Ninja 文件 |
| 跑 `MakeLZSA.exe` | 上游打包工具是 Windows 程序 | 生成内嵌 PAK |
| 跑我们编出来的 EXE | 在 Linux 上做冒烟测试 | 这就是第一次黑屏暴露问题的地方 |

注意：**编译本身不需要 Wine**。clang-cl、lld-link、ninja 都是 Linux 原生程序。

### 6.4 它不是模拟器，也不是虚拟机

| | Wine | QEMU（模拟器） | 虚拟机 |
| --- | --- | --- | --- |
| CPU 指令 | 本机直接执行 | 逐条翻译/动态翻译 | 本机虚拟化执行 |
| 需要 Windows 系统 | 不需要 | 需要装系统镜像 | 需要装系统镜像 |
| 性能 | 接近原生（除 API 开销） | 明显变慢 | 接近原生 |
| 兼容性 | 取决于 API 实现完整度 | 高（真 Windows） | 高（真 Windows） |

所以 Wine 的崩溃**不能直接当成 Windows 的结论**，反过来也一样。这正是 4.5 要你去真机取证的原因。

### 6.5 WINEPREFIX 是什么

一个目录就是一台"假电脑"：自己的 `C:` 盘、注册表、已装程序。
`WINEPREFIX=/tmp/spw-pfx` 让我们把测试完全隔离，不碰你系统里已有的 Wine 配置。
测试完整个目录可以删掉，等于"重装了一台干净的 Windows"。

## 7. 术语表

| 术语 | 含义 |
| --- | --- |
| Host / Target | 跑编译器的机器 / 跑产物的机器 |
| ABI | 二进制接口：调用约定、名字修饰、异常、结构体布局 |
| CRT | C/C++ 运行时（启动代码、`malloc`、异常、`std::`） |
| 导入库（.lib） | 只含"如何调用 DLL"的桩，链接时用来解析符号 |
| PE/COFF | Windows 的可执行/对象文件格式 |
| `/MT` | 静态链接 CRT，产物不依赖 VC 运行库 |
| MinGW | 用 GCC 工具链产出 Windows 程序，ABI 与 MSVC 不同 |
| Ninja | 只负责"按图执行命令"的构建器 |
| Premake | 生成构建图的工具（本项目用 Windows 版） |
| WINEPREFIX | Wine 的一台"假电脑" |

## 8. 自测题

1. 为什么说我们做的是交叉编译，虽然 CPU 架构没变？
2. `clang-cl` 和 `clang` 的区别是什么？
3. 没有微软 SDK 能不能交叉编译到 Windows？
4. 为什么第一次程序"编出来了"却没有界面？
5. `GfxCreate` 里明明有回退，为什么没生效？
6. 在 Windows 上取哪三样证据能判断是不是 Direct2D 的问题？
7. 改一个 `src/gui` 下的文件，最少的重建步骤是什么？
8. 为什么改了 `premake5.lua` 之后必须重新生成构建图？
9. Wine 里的崩溃能不能直接推断 Windows 也会崩？
10. 为什么"内存占用低"不能只靠调小缓存上限来验证？

答案要点见附录 C。

## 9. 下一步可以做的小实验

1. **真机取证**：用 GDI+ 版跑 `-log-to-file`，把日志前 50 行贴出来，确认 `InitDirect2D` 是否被调用。
2. **恢复 Direct2D 做 A/B**：把 `gUseDirect2D` 改回 `true` 编一版，只在 Windows 上测；若崩，取 .dmp 看模块。
3. **渲染后端对比**：同一页分别用 D2D/GDI+ 渲染成 PNG，比较文本边缘与耗时。
4. **构建图实验**：故意改一个库名大小写，观察报错信息，理解 `adapt-ninja.py` 的必要性。
5. **缓存预算原型**：先只加一个"全局字节上限"的统计日志，不改行为，观察真实用量分布。

---

# 第二部分：这段路是怎么走过来的

## 2.1 起点：一个被误会的软件

最初你以为 SumatraPDF 是闭源的，想研究 PDF-XChange 的"低内存"到底怎么实现。
后来发现 SumatraPDF 开源、代码可读，方向才变成"读源码 + 自己改"。
这个转折很重要：**从"猜别人的实现"变成"能验证的实现"**。

## 2.2 约束逼出来的方案

没有 Visual Studio、磁盘紧张、不想装几个 GB 的工具——这些约束本来像障碍，
结果把方案推向了"在 Linux 上交叉编译"。很多人卡在这里就放弃了，你选择继续。
回头看，这条路的技术含量恰恰来自约束：**不能装 VS，就必须理解 CRT/SDK/ABI 到底是什么**。

## 2.3 第一次成功是"假成功"

Ninja 退出码 0、EXE 生成、PE 头正确——很容易在这里宣布胜利。
但真正的验收是"窗口里出现 PDF 页面"。第一次在 Wine 里：窗口有了、页数识别了、然后进程死了、截图全黑。
这就是工程上最常见的错觉：**构建成功 ≠ 程序可用**。

## 2.4 黑屏那天最关键的一步

不是"再编一次"，而是**做对照**：关掉 `d2d1.dll` 再跑，截图不黑了。
一个变量、一次对照，把"玄学崩溃"变成了"某条绘制路径的问题"。
这个方法后来贯穿了整个项目：内存实验也是"只改一个常量，其他全同"。

## 2.5 一行代码的转折

修复不是重写渲染器，而是把默认后端从 Direct2D 换成项目自带的 GDI+：一行 `true` → `false`。
经验：**先用最小改动让程序可用，再决定要不要修根因**。
代价要说清楚：Direct2D 路径的缺陷还在，只是不再默认走它。

## 2.6 从"能用"到"可靠"

接下来你没有停在"能打开 PDF"，而是提了三个很具体的问题：异常关闭会丢会话、要记录最后打开时间、多窗口要能一起恢复。
这三件事的共同点是：**都属于持久化与恢复语义**，不是画界面。
实现上因此有了定时快照、`LastOpenedUtc` 字段、恢复期间禁止写盘的守卫——每一条都能在 Wine 里用脚本验证。

## 2.7 负结果也是结果

MuPDF 缓存上限从 256 MiB 调到 64 MiB，RSS 没有下降（130,964 / 132,096 对 132,252 / 132,300 KB）。
这不是失败，而是排除了一条错误路径，并且顺带确认了 PDF-XChange 的低占用来自"按文档增删缓存 + 空闲淘汰"，
而不是"更小的统一上限"。

## 2.8 沉淀下来的四条方法

1. **证据分层**：观察 / 推导 / 未知，写结论时标明属于哪一层。
2. **最小变量**：一次只改一个东西，否则无法归因。
3. **隔离环境**：独立 `WINEPREFIX`、独立 `-appdata`、独立测试目录，避免上一次的残留污染结论（我们就遇到过残留窗口导致"恢复出 4 个窗口"的假象）。
4. **可复现**：所有改动落成补丁，所有测试落成脚本，换台机器能重来。

## 2.9 心态

第一次没有界面，不代表方向错了；它只说明"运行时还有一层没验证"。
真正浪费时间的是把"编过了"当成"做完了"，以及在没有对照的情况下反复重编。

---

## 附录 A：常用命令

```sh
# 构建
cd /cluster/home/it_stu171/rea/sumatrapdf-build && bash build-static.sh

# 看产物结构
/cluster/home/it_stu171/rea/sumatrapdf-toolchain/llvm19/bin/llvm-readobj \
  --coff-imports --file-headers tree/out/rel64/SumatraPDF-static.exe | head -40

# 隔离环境跑 GUI
export WINEPREFIX=/tmp/spw-pfx DISPLAY=:97
Xvfb :97 -screen 0 1400x1000x24 &
wine tree/out/rel64/SumatraPDF-static.exe -appdata "Z:\\tmp\\sp-appdata" file.pdf

# 抓日志
wine tree/out/rel64/SumatraPDF-static.exe -log-to-file "Z:\\tmp\\sp.log" file.pdf

# 命令行渲染
wine tree/out/rel64/SumatraPDF-static.exe -render -zoom 1 "Z:\\tmp\\t1.pdf"
```

## 附录 B：证据索引

| 事实 | 在哪能看到 |
| --- | --- |
| 工具链组成与版本 | `sumatrapdf-toolchain/TOOLCHAIN-INFO.md` |
| 构建脚本 | `sumatrapdf-build/build-static.sh` |
| 构建图修补 | `sumatrapdf-build/adapt-ninja.py` |
| 资源打包 | `sumatrapdf-build/pack-static.py` |
| 本地改动与补丁 | `sumatrapdf-build/local/` |
| 构建与测试记录 | `sumatrapdf-build/README-zh.md` |
| 图形后端开关 | `../sumatrapdf/src/gui/Gfx.cpp`（`gUseDirect2D`） |
| GDI+ 补丁 | `local/Gfx.patch` |
| 崩溃转储规则 | `src/base/CrashHandler.cpp`、`src/SumatraPDF.cpp`（`GetCrashInfoDirTemp`） |

## 附录 C：自测题答案要点

1. 因为可执行格式、ABI、系统 API 都变了，只有 CPU 架构没变。
2. `clang-cl` 用 MSVC 的命令行与 ABI 习惯，`clang` 用 GCC/Itanium ABI。
3. 不能。编译器不带微软的头文件、导入库与 CRT。
4. 编译链接都对，但运行时走了 Direct2D 绘制路径并在其中崩溃。
5. 回退只覆盖 `GfxCreate`，且崩溃发生在 DLL 内部时进程直接结束，回退没有机会执行。
6. `-log-to-file` 的 `InitDirect2D` 行、`crashinfo/*.dmp`、WER 报告。
7. 改 `local/` 副本 → `bash build-static.sh`（只重编该文件并链接）。
8. 因为 Ninja 图由 Premake 生成，源文件列表变化不会自动进入旧图。
9. 不能。Wine 的 API 实现完整度与真 Windows 不同。
10. 因为缓存上限只是"允许用的上限"，小文档根本用不到上限，必须实测 RSS 曲线。

---

*本文由本项目实际执行过程整理；所有版本号、校验值、路径均取自本机现存文件与日志。*

---

# 附录 D：零基础补课——每个零件到底在干什么

## D.1 你在 Visual Studio 里按的那一下，其实做了五件事

写一个黑窗口 Hello World 时，VS 的"生成"按钮背后依次做了：

| 步骤 | 你看到的现象 | 实际动作 |
| --- | --- | --- |
| 预处理 | 无 | 展开 `#include`、宏 |
| 编译 | 无 | 每个 `.cpp` 变成一个 `.obj`（机器码 + 符号表） |
| 汇编 | 无 | `.asm` 文件单独汇编成 `.obj` |
| 链接 | 无 | 把一堆 `.obj` 和 `.lib` 拼成一个 `.exe` |
| 打包 | 无 | 写入资源（图标、版本信息、内嵌文件） |

**交叉编译没有改变这五步，只是把执行它们的程序换成了 Linux 上的版本。**
这就是为什么你觉得"没体会到"——因为你在 VS 里也从没见过它们。

## D.2 五个角色，一句话一个

| 角色 | 通俗解释 | 我们的具体材料 | 缺了会怎样 |
| --- | --- | --- | --- |
| 编译器 | 把 C++ 翻译成机器码，并规定"函数名怎么拼写" | `clang-cl` | 没有 `.obj` |
| 头文件 | 告诉编译器"系统提供哪些函数、长什么样" | Windows SDK 的 `include/` | 报"找不到 windows.h" |
| 导入库 | 告诉链接器"这个函数在哪个 DLL 里、怎么调用" | SDK 的 `.lib` | 报"无法解析的外部符号" |
| CRT | 提供 `main` 之前的启动代码、`malloc`、`std::string`、异常 | 微软 CRT（`/MT` 时整包进 EXE） | 程序没有入口，或运行时缺 DLL |
| 链接器 | 把 `.obj` + `.lib` 拼成一个 PE 文件，填地址 | `lld-link` | 只有零件，没有成品 |

一个类比：

- **编译器** = 把中文图纸翻译成施工队能看懂的图；
- **头文件** = 国家标准目录（告诉你插座是什么规格）；
- **导入库** = "这个插座由市政供电"的接线说明；
- **CRT** = 通用的水电工班底（开工前的准备、收尾、应急）；
- **链接器** = 总装厂，把零件和说明书拼成一栋能交付的房子。

## D.3 为什么"交叉编译变成了 Windows SDK"

编译器只负责"翻译 + 生成机器码"，它**不携带**任何一家操作系统的头文件和库。
你要生成 Windows 程序，就必须拿到：

1. 微软的**头文件**（编译期知道 `CreateWindowW` 的参数）；
2. 微软的**导入库**（链接期知道它来自 `user32.dll`）；
3. 微软的**CRT**（`main` 之前谁初始化堆、谁调用你的 `main`）。

这三样只能来自微软，所以"在 Linux 上编 Windows 程序"必然表现为"下载并使用 Windows SDK/CRT"。
`xwin` 只是帮你把这三样按微软官方清单下载下来，不参与编译。

## D.4 `/MT` 到底做了什么

| 选项 | 含义 | 产物 |
| --- | --- | --- |
| `/MD` | 动态链接 CRT，运行时找 `VCRUNTIME140.dll` | 小，但要装运行库 |
| `/MT` | 静态链接 CRT，把运行时打进 EXE | 大一点，但**单文件可分发** |

我们的 21.8 MB EXE 里就包含了 CRT 和一整套第三方库（MuPDF、FreeType、HarfBuzz、libjpeg-turbo…），
所以导入表里只有系统 DLL。这就是"单个 EXE"的技术含义。

## D.5 汇编器、构建生成器、构建执行器

| 工具 | 属于哪一步 | 为什么需要 |
| --- | --- | --- |
| NASM | 汇编 | 少数模块是手写汇编（如 unrar 的 SSSE3 优化） |
| Premake | 生成构建图 | 上游用它描述"哪些源文件、哪些库、什么选项" |
| Ninja | 执行构建图 | 只负责"按图跑命令、只重跑该跑的" |
| MakeLZSA | 资源打包 | 把字体/翻译/JS 压成内嵌 PAK |

注意 Premake 和 MakeLZSA 是 Windows 程序，所以要用 Wine 跑；**编译和链接本身是 Linux 原生程序**。

## D.6 你只需要记住这五句话

1. 编译 = 每个源文件变成零件（`.obj`）。
2. 链接 = 零件 + 库拼成成品（`.exe`）。
3. CRT 和 SDK 决定"能不能在 Windows 上跑"，编译器只决定"代码对不对"。
4. 静态链接（`/MT`）= 把运行库塞进 EXE，换来单文件分发。
5. 报错先看它发生在哪一步：编译错（语法/头文件）、链接错（缺库/符号）、运行错（环境/逻辑）。

---

# 附录 E：把 PDF-XChange 的灰色搬过来

## E.1 实测取色（不是猜的）

在隔离显示上用 Wine 打开 PDF-XChange Editor 11，缩放到页面小于窗口，截图后按像素统计：

| 部位 | 实测颜色 | 说明 |
| --- | --- | --- |
| 页面纸张 | `#FFFFFF` | 文档本身的白色底 |
| 页面周围的画布 | `#9B9893`（155,152,147） | 你想要的"灰底" |
| 工具栏/标签区 | `#DAD7D0`（218,215,208） | 偏暖的浅灰 |
| 边框/分隔线 | `#BEBBB4`（190,187,180） | 比工具栏略深 |

作为对照，我们这个构建的默认画布是接近白的 `#F7F3F7`，所以两者观感差别明显。

## E.2 在 Sumatra 里怎么设置（已实测生效）

编辑 `SumatraPDF-settings.txt`，加入：

```ini
FixedPageUI [
    WindowBgCol = #9b9893
]
MainWindowBackground = #dad7d0
```

含义：

| 设置 | 作用 |
| --- | --- |
| `FixedPageUI.WindowBgCol` | PDF 画布（页面周围）的背景色 |
| `MainWindowBackground` | 主窗口底色（标签栏/边角） |
| `FixedPageUI.BackgroundColor` | 页面纸张颜色（默认 `#ffffff`，不要和上面混） |

实测：应用后画布像素为 `#9C9994`（156,153,148）。与取色值 `#9B9893` 的 1 个色阶差异来自 VNC 的 16 位色深，不是设置错误。

## E.3 想要更接近的观感，可以再调三项

| 参数 | 建议 | 效果 |
| --- | --- | --- |
| `FixedPageUI.PageSpacing` | `8 8` | 页与页之间留白，接近编辑器的分页感 |
| `FixedPageUI.WindowMargin` | `6 8 6 8` | 页面与窗口边缘的距离 |
| `MainWindowBackground` | `#dad7d0` | 与工具栏同色，减少割裂感 |

注意：这些是"主题"层面的个人偏好，不会改变渲染结果，也不影响性能。

---

# 附录 F：我们目前对 PDF-XChange 了解到哪一步

## F.1 已经确认（有文件/截图/文档为证）

| 层面 | 结论 | 证据 |
| --- | --- | --- |
| 打包结构 | MSI 402 MB，解包 1.1 GB，212 个 PE 模块 | `msiextract` 结果 |
| 主程序 | `PXCEditor.exe` 仅 1.67 MB，只依赖系统 DLL | 导入表 |
| 核心 | `PXCEditorCore.x64.dll` 76.3 MB，只导出 9 个符号 | `llvm-readobj` |
| 插件化 | `Plugins.x64/` 下 22 个 `.pvp`（实为 PE DLL），按需 `LoadLibrary` | 文件类型 + 字符串 |
| 进程外 | `PXC.ProcessPool.Worker.x64.exe`、`PXCHost64.exe` | 文件清单 |
| 内存工具 | `Microsoft.IO.RecyclableMemoryStream.dll` 3.0.1.0 | 版本串 |
| 脚本引擎 | 内嵌 SpiderMonkey | 符号字符串 |
| 文本排版 | 依赖 ICU 数据（缺 `icudt78l.dat` 会报 UniLayout 错误） | 实际报错 |
| 缓存策略 | 页面缓存，默认上限为可用物理内存的 25%，可设 MB 与 TTL，渲染/缩略图线程数可配 | 厂商 KB230 与 V11 帮助 |
| 释放行为 | 最小化约 30 秒后释放、关闭文档立即释放 | 厂商文档（未在 Wine 复现） |

## F.2 只是推断（合理但未证实）

- 插件按需加载是它"启动快、空闲占用低"的主要原因；
- 进程池用于把转换/OCR 这类高内存任务移出 UI 进程；
- 你 Windows 上那次打不开，与 Direct2D 路径有关（Wine 侧成立，真机未证实）。

## F.3 还不知道

- 内部数据结构与页面缓存的具体实现（我们没做反编译级分析）；
- 为什么在 Wine 里 `Ctrl+O` 会让它崩；
- 真机上多标签的内存曲线（Wine 里每次启动都是新进程，测不到）；
- 各主题配色的完整色板（只取了默认浅色主题的几个点）。

## F.4 历史功能怎么借鉴

PDF-XChange 的"最近文件/历史"和 Sumatra 首页的最近记录，用途相同：**快速回到上次看的文档**。
差别在呈现与信息量：前者是列表（可固定、可分组），后者是带缩略图的起始页。
我们现在已经有 `LastOpenedUtc`，可以直接做出"按最后打开时间排序"的历史视图，
这比单纯复刻外观更有价值——它把"记录时间"变成了可用信息。


## F.5 案例：把 Ctrl+B 从「收藏」改成「书签」（一次完整改动）

这是"日常开发落在第一行"的真实样本，用来演示从**读源码定位语义**到**改一处行为**再到**验证**的闭环。

| 步骤 | 做了什么 | 结论 |
| --- | --- | --- |
| 1 定位 | 查 `KeyboardHelpCommon.h`：`Ctrl+B = CmdFavoriteAdd`、`F12 = CmdToggleBookmarks` | Ctrl+B 根本不叫 bookmark；F12 才是，且只读 |
| 2 判断可行性 | Sumatra 没有 PDF 写出器，改不了 `/Outlines` | 只能把「收藏」这个既有数据结构当书签用 |
| 3 设计 | 在 `TableOfContents.cpp` 里加一个 `TreeModel` 包装器，把书签排在大纲前面 | 渲染层完全复用，行外观/滚动/展开全免费 |
| 4 风险控制 | 文件没有书签时**直接返回原 `TocTree`** | 老路径零改动，回归面收敛到「有书签的文档」 |
| 5 实现 | 6 个文件、7 个补丁（模型、右键菜单、可用性判断、菜单文案）| 最大一个 419 行，其余都在 30 行内 |
| 6 验证 | Wine + Xvfb 截图 + 设置文件断言：加/点/删各一遍 | 发现并修掉「删书签后旧行不消失」的重绘问题 |

**教学点**：真正的成本不在写代码，而在第 1 步和第 6 步——先确认"用户说的 bookmark 和程序里的 bookmark 不是同一个东西"，
再用像素级证据排除"我以为只是没刷新"的误判（`TVM_GETCOUNT` 已经是 3，说明数据对、只是没重绘；
最后靠"重建前隐藏控件、重建后显示"解决，改动只有三行）。

---

# 附录 G：和 CMake 项目（ORB-SLAM3）比，差在哪