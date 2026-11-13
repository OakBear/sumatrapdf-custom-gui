# SumatraPDF 本地改造总览：架构、修改与编译

> 本文把三件事合成一份可检索的文档：**上游架构**、**我们改了什么**、**怎么编译出来的**。
> 证据约定：**观察**（有代码/日志/测量为证）、**推导**（由代码逻辑推出）、**未知**（未验证）。

## 0. 三十秒版本

| 项 | 内容 |
| --- | --- |
| 目标 | 在 Linux 上产出可在 Windows 运行的单个 EXE，并加入会话可靠性、时间戳、缓存上限等改造 |
| 上游 | SumatraPDF `075b05ce7cfe3fd0ce1eec9cdff2ae8ea00c6e53`，未改动 |
| 产物 | `sumatrapdf-build/tree/out/rel64/SumatraPDF-static.exe`，21,796,864 字节（含 PDF 书签脚本的内嵌资源包，见 §10）|
| 校验值 | SHA-256 `3a626d1155f0c0318ea404d8f8f0a4cc7a185c3072796f35ad39010c56f14c8b`（同源码重建哈希可能不同：PE 头 `TimeDateStamp` 每次链接都会变；加入书签脚本前的版本哈希为 `13ca795be2a0d64c7eb65aabf82a71a479f6b6581ad34725f74606c0b239dc49`，备份为 `tree/out/rel64/SumatraPDF-static-no-bookmark-scripts.exe`）|
| 工具链 | LLVM 19.1.7（clang-cl/lld-link/llvm-lib/llvm-rc）+ 微软 CRT 14.44.17.14 + Windows SDK 10.0.26100（xwin 下载）|
| 构建方式 | Wine 跑上游 Premake/MakeLZSA，Linux 原生 Ninja 驱动编译与静态链接 |
| 验证级别 | Wine 运行时验证通过；Windows 11 真机仅人工确认可用，崩溃路径未取证 |

## 1. 产物与差异一览

额外补丁全部在 `sumatrapdf-build/local/`，每个都有同名 `.patch`，可从上游原文逐字节重建：

| 补丁 | 上游文件 | 目的 |
| --- | --- | --- |
| `Gfx.patch` | `src/gui/Gfx.cpp` | 默认图形后端由 Direct2D 改为 GDI+（绕开崩溃路径）|
| `Settings.patch` / `Settings_h.patch` | `src/Settings.cpp` / `Settings.h` | 文档状态新增 `LastOpenedUtc` |
| `SumatraPDF.patch` | `src/SumatraPDF.cpp` | 时间戳记录、定时快照、恢复守卫、缓存定时清理 |
| `AppSettings.patch` | `src/shared/AppSettings.cpp` | 恢复期间禁写、原子写入与长路径回退、保留时间戳字段 |
| `RenderCache.patch` / `RenderCache_h.patch` | `src/shared/RenderCache.{h,cpp}` | 缓存字节预算 + 空闲清理 |
| `EngineMupdf.patch` | `src/shared/EngineMupdf.cpp` | MuPDF store 由 256 MiB 改为 64 MiB（实验，未显示收益）|
| `unrar.patch` / `heic.patch` / `libwebp.patch` / `ReadAloud_win.patch` | `ext/` 与 `src/shared/` | 让 clang-cl 能编译（SSSE3 调用约定、SIMD、`Color` 宏冲突）|
| `TableOfContents.patch` / `FavoritesCommon.patch` / `AddFavoriteDialog.patch` / `SidebarPanel.patch` / `Menu.patch` / `MenuDefs.patch` / `Commands.patch` | `src/TableOfContents.cpp`、`src/AddFavoriteDialog.cpp`、`src/SidebarPanel.cpp`、`src/Menu.cpp`、`src/shared/{FavoritesCommon,MenuDefs,Commands}.cpp` | Ctrl+B 书签合并进 F12 面板，并提供四个「加书签」入口（见 §3.8）|

## 2. 上游架构速览

### 2.1 目录分层

| 层 | 位置 | 职责 |
| --- | --- | --- |
| 基础库 | `src/base/` | 字符串/文件/窗口/线程/日志/PE 资源等基础设施，跨平台 |
| GUI 框架 | `src/gui/` | 自绘控件、布局、绘图后端（Direct2D/GDI+）、平台抽象 |
| 文档与渲染 | `src/shared/`（322 个文件）| 引擎封装、`DisplayModel`、`DocController`、`RenderCache`、搜索、注释、会话 |
| 应用层 | `src/*.cpp` | 主窗口、标签页、对话框、命令、安装器、崩溃处理 |
| 第三方 | `ext/`（1331 个 C/C++ 源文件）| MuPDF、FreeType、HarfBuzz、libjpeg-turbo、OpenJPEG、JBIG2、Tesseract 等 |

### 2.2 关键对象关系

```text
MainWindow ── owns ──> WindowTab ──> DocController ──> DisplayModel ──> EngineBase
     │                     │                │                 │              │
     │                     │                │                 │              └─ EngineMupdf（每文档一个 fz_context）
     │                     │                │                 └─ 页面布局、可见性、缩放
     │                     │                └─ 导航/搜索/选区
     │                     └─ 标签状态、文件路径、加载状态
     └─ 菜单/工具栏/画布

全局单例：RenderCache（渲染线程池 + 位图缓存）、gWindows、gSettings
```

### 2.3 两条主要数据流

**打开文档**：命令行/文件对话框 → `LoadDocument` → 猜类型 → 创建 Engine → `DisplayModel` → `LoadDocumentFinish`（写历史、更新界面）→ `RenderCache::RequestRendering` → 渲染线程产位图 → 画布绘制。

**会话持久化**：状态变化 → `ScheduleSaveSettings()`（合并短时间内的多次请求）→ `SaveSettings(force)` 序列化 `gSettings` → 写 `SumatraPDF-settings.txt`；启动时读取并 `RestoreSession`。

## 3. 我们的修改

### 3.1 图形后端默认 GDI+

| 项 | 内容 |
| --- | --- |
| 位置 | `local/Gfx.cpp`（上游 `src/gui/Gfx.cpp:345`）|
| 改动 | `bool gUseDirect2D = true;` → `false` |
| 原因 | Direct2D 路径在 Wine 与用户的 Windows 11 上都表现为启动即失败/黑屏 |
| 回退 | 项目自带 GDI+ 实现（`GfxGdiplus`），功能等价、性能略低 |
| 证据 | 禁用 `d2d1` 后 Wine 不再崩溃；GDI+ 版可正常渲染 |
| 风险 | 未修复 Direct2D 缺陷，只是不再默认走它；高 DPI 下文字度量略有差异 |

### 3.2 记录最后打开时间

| 项 | 内容 |
| --- | --- |
| 数据 | `FileState.lastOpenedUtc`（UTC 字符串，形如 `2026-11-07T13:38:51Z`）|
| 声明 | `local/Settings.h` |
| 注册 | `local/Settings.cpp:844`：字段表新增一项，`fieldCount` 36 → 37，名字表追加 `LastOpenedUtc` |
| 写入 | `local/SumatraPDF.cpp:3387` `RememberPdfViewed()`：只对真实 PDF、非临时文件、非快速预览记录 |
| 触发点 | 打开完成（`LoadDocumentFinish`）、切换标签（`LoadModelIntoTab`）、恢复会话后每个窗口的当前标签（`SumatraPDF.cpp:17527`）|
| 保留策略 | `AppSettings.cpp:1839`：清理历史时即使 `RememberStatePerDocument=false` 也保留该字段 |

守卫条件（代码原样）：

```cpp
if (!tab || (!duringStartup && gIsStartup) || gPluginMode || tab->skipHistory ||
    !tab->win || tab->win->isQuickLook || !SettingsRememberOpenedFiles() ||
    IsOpenCachePath(tab->filePath) || path::IsEphemeralHostFile(tab->filePath) ||
    !tab->AsFixed() || !EngineMupdfIsPdf(tab->AsFixed()->GetEngine()))
    return;
```

### 3.3 定时快照

| 项 | 内容 |
| --- | --- |
| 位置 | `local/SumatraPDF.cpp:15258` `OnSessionSnapshotTimer`、`:17630` 创建定时器 |
| 周期 | 3 分钟（`SetTimer(nullptr, 0, 3*60*1000, ...)`）|
| 条件 | `!gIsStartup && !gDontSaveSettings && len(gWindows) > 0` 才写盘 |
| 附带 | 同时调用 `gRenderCache->TrimIdleEntries()`（见 3.6）|
| 证据 | 把周期临时改为 10 秒的测试版：36 秒内日志 3 次触发，每次伴随 `SaveSettings` |
| 风险 | 定时器是线程定时器，不随窗口关闭取消；程序退出时显式 `KillTimer` |

### 3.4 多窗口恢复守卫

| 项 | 内容 |
| --- | --- |
| 标志 | `gRestoringSessionSnapshot`（定义在 `AppSettings.cpp:86`，`SumatraPDF.cpp:3384` 引用）|
| 置位 | `SumatraPDF.cpp:17338`，进入恢复循环前 |
| 禁写 | `AppSettings.cpp:707`：`SaveSettings()` 在恢复期间直接返回，避免半恢复状态覆盖磁盘 |
| 解除 | `SumatraPDF.cpp:17444`：所有窗口恢复成功后解除并立即 `ScheduleSaveSettings()` |
| 代价 | 恢复被中断（会话代数变化/窗口被关）时本次运行不再保存设置——有意的保守策略 |
| 证据 | 两窗口各自打开 PDF，退出后 `SessionData` 含 2 个窗口；重启恢复出 2 个窗口 |

### 3.5 设置文件的原子写入与长路径回退

| 项 | 内容 |
| --- | --- |
| 位置 | `local/AppSettings.cpp:786-812` |
| 做法 | 同目录临时文件写入 → `file::RenameReplace` 原子发布；失败删除临时文件，旧设置保持完整 |
| 长路径 | `GetTempFileNameW` 受 `MAX_PATH` 限制；失败时回退为直接就地写入并记日志 |
| 原因 | 断电/强杀时避免留下半个设置文件；长配置目录下也不能让保存静默失效 |
| 风险 | 回退路径不是原子的（与上游行为一致）|

### 3.6 渲染缓存字节预算 + 空闲清理（借鉴 PDF-XChange）

| 项 | 内容 |
| --- | --- |
| 位置 | `local/RenderCache.{h,cpp}` |
| 预算 | `CacheBudgetBytes()` 返回 256 MiB（软上限）|
| 统计 | 每个条目 `byteSize`，`RenderCache.cachedBytes` 随增删维护 |
| 淘汰顺序 | 同文档不可见页 → 旧缩放/旋转 → 屏幕外瓦片 → 其他文档中**最久未绘制**条目 |
| 空闲清理 | `lastUse` 超过 5 分钟且页面不可见则丢弃；`TrimIdleEntries()` 由 `Add()` 与 3 分钟定时器调用 |
| 安全边界 | 只丢缓存；`refs>1` 与当前可见页永不驱逐；不触碰文档/注释 |
| 证据 | 64 MiB 实验版连翻 120 页：RSS 194 → 142 MB；TTL 20 秒实验版静置 70 秒：130 → 122 MB，日志 `dropped 13 idle entries (29 MB)` |
| 现状 | 正式版 256 MiB 在该负载下无变化（工作集约 66 MB），作用是限制最坏情况 |

### 3.7 MuPDF store 上限实验（负结果）

| 项 | 内容 |
| --- | --- |
| 位置 | `local/EngineMupdf.cpp:3535`：`fz_new_context(..., 64 MiB)`（上游为 `FZ_STORE_DEFAULT` 256 MiB）|
| 假设 | 更小的逐文档缓存能降低多文档内存 |
| 实测 | 5 个 PDF，256 MiB vs 64 MiB：130,964/132,096 对 132,252/132,300 KB，无差异 |
| 结论 | 该预算只在文档大到接近 256 MiB 时才生效；当前保留实验值，可改回 `FZ_STORE_DEFAULT` |

### 3.8 书签（Ctrl+B）语义改造

上游 Ctrl+B 是 `CmdFavoriteAdd`「Add to favorites」，写 settings 的 `Favorites` 段；F12 才是「Bookmarks」，显示的是 PDF 自带 `/Outlines`，只读。Favorites 面板默认关闭，所以存了书签界面毫无反馈。本次把 Favorites 当作**用户书签**，合并进 F12 面板。

| 项 | 内容 |
| --- | --- |
| 位置 | `local/TableOfContents.cpp`（主体）、`local/shared/FavoritesCommon.cpp`、`local/AddFavoriteDialog.cpp`、`local/SidebarPanel.cpp`、`local/Menu.cpp`、`local/shared/{MenuDefs,Commands}.cpp` |
| 模型 | `BookmarksModel : TreeModel`，`ChildCount/ChildAt` = 书签项 + `/Outlines` 各项；书签用 `TocItem` 包装，`id >= kBookmarkItemIdBase` 区分，`destNotOwned` 避免释放引擎数据 |
| 交互 | 点击书签行 → `JumpToFavorite()`（页码 + 滚动位置 + 章节）；右键 →「Remove Bookmark」→ `DelFavorite()` |
| 添加入口 | `Ctrl+B`；页面右键「Add Bookmark (page N)　Ctrl + B」；**书签面板空白处右键「Add Bookmark (page N)」**；菜单栏 Bookmarks → Add Bookmark。全部走 `CmdFavoriteAdd`，命名框按钮已改为 `Add Bookmark` |
| 可用性 | `IsSidebarViewAvailable(Bookmarks)` 与菜单 `HasToc()` 判断放宽为 `HasToc() \|\| HasUserBookmarks()`，无目录文档也能有书签面板 |
| 自动展开 | 首个书签落盘后按 `ShowToc` 自动打开面板 |
| 刷新 | `UpdateFavoritesTreeForAllWindows()` 挂钩 → `uitask::Post` 延后重建；重建前 `ShowWindow(SW_HIDE)`、之后 `SW_SHOW` |
| 零回归 | 文件无书签时直接返回原 `TocTree`，模型与上游完全一致 |
| 未做 | 不写回 PDF（要写 `/Outlines` 走 §10 的 MuPDF JS 路线）；过滤框不参与过滤书签；无重命名/拖拽 |
| 证据 | 面板空白处右键 → Add Bookmark (page 2) → 命名框 [Add Bookmark] → 面板顶部出现 `demo 2`；无目录 200 页 PDF：Ctrl+B 后自动出现 `beta 4`；自制目录 PDF：`alpha 2` 在 `Chapter 1/1.1/2` 之上；点击从第 1 页跳到第 2 页；右键删除后设置清空、行消失、目录 3 行无重影；无目录无书签文档 F12 行为与上游一致 |

踩到的坑：删除书签时若直接删行重建，可见的原生 TreeView 会保留旧行（删掉的那行还在，整体上移一行，同时下方面板变黑）。`TVM_GETCOUNT` 已经是 3，说明数据正确、只是没有重绘；用 `RedrawWindow`（含 `RDW_UPDATENOW`、整帧）都无效，最后靠重建时**先隐藏控件再显示**解决。

### 3.9 clang-cl 兼容补丁

| 补丁 | 问题 | 处理 |
| --- | --- | --- |
| `unrar.patch` | SSSE3 函数在未检测 CPU 时被调用 | 仅在调用方确认支持后执行 |
| `heic.patch` / `libwebp.patch` | Clang SIMD 路径在交叉编译下不可用 | 改用原有标量实现（像素效果待验证）|
| `ReadAloud_win.patch` | WinRT 头的 `Color` 与项目类型冲突 | 局部改名 |

## 4. 会话恢复时序

```text
启动
  ├─ 读 settings → gInitialSessionData
  ├─ restoreSession ? 对每个窗口:
  │     CreateAndShowMainWindow → RestoreTabOnStartup → TabsSelect
  │     （此期间 gRestoringSessionSnapshot = true，SaveSettings 直接返回）
  ├─ 全部成功 → gRestoringSessionSnapshot = false → ScheduleSaveSettings
  └─ 任一失败/被中断 → 保持 true，本次运行不再写设置

运行中
  ├─ 打开/切换标签 → RememberPdfViewed → lastOpenedUtc → ScheduleSaveSettings
  └─ 每 3 分钟 → OnSessionSnapshotTimer → SaveSettingsNow + TrimIdleEntries
```

## 5. 编译体系

### 5.1 工具链

| 组件 | 版本 | 来源 |
| --- | --- | --- |
| clang-cl / lld-link / llvm-lib / llvm-rc | 19.1.7 | conda-forge（清华镜像），micromamba 安装 |
| MS CRT | 14.44.17.14 | `xwin 0.10.0` 从微软 VS 清单下载 |
| Windows SDK | 10.0.26100 | 同上 |
| Ninja | 1.13.2 | 官方 release |
| NASM | 2.16.03 | 源码本地编译 |
| Wine | 6.0.2 | 系统安装，用于 Premake / MakeLZSA / 运行测试 |

### 5.2 流水线

```text
premake5.exe (Wine) → tree/.work/ninja/*.ninja
        ↓ adapt-ninja.py（路径重定向、丢弃不可用目标、库名大小写别名）
tree/.work/linux/*.ninja
        ↓ ninja -j4
clang-cl /c → .obj ；llvm-lib → .lib ；MakeLZSA (Wine) → 内嵌 PAK
        ↓ lld-link /MT
out/rel64/SumatraPDF-static.exe
```

规模（实测）：rel64 配置 280 个构建目标；最终链接 276 个 `.obj` + 27 个 `.lib` → 1 个 EXE；整个图含所有配置 5961 条 build 语句。

### 5.3 单文件是怎么来的

- `/MT` 静态 CRT；所有第三方库以 `.lib` 形式静态链接；
- 导入表只有系统 DLL（`KERNEL32`、`USER32`、`gdiplus`…），没有 `VCRUNTIME140.dll`、没有 `mupdf.dll`；
- 字体/翻译/JS 通过 `pack-static.py` 打成 PAK 资源嵌入。

### 5.4 本地文件是怎么进入构建的

| 方式 | 文件 | 说明 |
| --- | --- | --- |
| 符号链接到 `local/` | `Settings.h`、`Settings.cpp`、`SumatraPDF.cpp`、`shared/AppSettings.cpp`、`shared/EngineMupdf.cpp`、`shared/RenderCache.{h,cpp}` | `tree/src` 下是指向 `local/` 的绝对符号链接，改 `local/` 即改构建输入 |
| 构建图重定向 | `gui/Gfx.cpp`、`shared/ReadAloud_win.cpp` | `adapt-ninja.py` 把输入路径改到 `local/` |
| 上游原样 | 其余文件 | `tree/src` 指向 `sumatrapdf/src` |

因此“改代码”的入口永远是 `local/`；`tree/src` 不要直接编辑（很多是链接）。

### 5.5 五个坑

| 坑 | 表现 | 处理 |
| --- | --- | --- |
| 改了 `premake5.lua`/增删源文件 | Ninja 不知道 | 重跑 Premake + `adapt-ninja.py` |
| 目录移动 | 绝对路径失效 | 重建符号链接与相对布局 |
| 库名大小写 | `Rstrtmgr.lib` vs `rstrtmgr.lib` | `build-static.sh` 的别名目录 |
| 新增依赖 | 缺头/缺库 | 用 xwin SDK 或自编 `.lib`，保持 `/MT` |
| 只在 Wine 验证 | 真机行为不同 | 关键改动必须在 Windows 复测 |

## 6. 验证体系

| 层级 | 问题 | 手段 |
| --- | --- | --- |
| L1 | 编过没有 | Ninja 退出码、产物存在 |
| L2 | 是否合法 PE | `llvm-readobj --file-headers --coff-imports` |
| L3 | 能启动吗 | 隔离 `WINEPREFIX` + `-appdata` 独立目录 |
| L4 | 画出来了吗 | `Xvfb` + `import` 截图 + 像素统计/OCR |
| L5 | 交互/语义对吗 | `xdotool` 发按键、读设置文件、日志断言 |

已执行的关键测试：

| 测试 | 结果 |
| --- | --- |
| 双窗口保存/恢复 | 2 个窗口，`LastOpenedUtc` 2 条 |
| 定时快照（10 秒测试版）| 36 秒 3 次触发 + `SaveSettings` |
| 缓存预算（64 MiB 测试版）| 连翻 120 页，缓存稳定 28 项/66 MB，RSS 194→142 MB |
| 空闲清理（20 秒测试版）| `dropped 13 idle entries (29 MB)`，RSS 130→122 MB |
| MuPDF store 对照 | 256 vs 64 MiB 无差异 |
| 页面渲染 | 截图含白底、灰边、红/蓝矢量块 |
| 书签合并（无目录 PDF）| Ctrl+B 后面板自动出现 `beta 4`，点击跳回第 4 页 |
| 书签合并（有目录 PDF）| `alpha 2` 在 `Chapter 1 / 1.1 / Chapter 2` 之上，点击第 1 页→第 2 页 |
| 删除书签 | 设置清空、行消失、目录 3 行无重影（重建前后 `TVM_GETCOUNT` 4→3）|

## 7. 补丁与复现

```sh
cd /cluster/home/it_stu171/rea/sumatrapdf-build
patch --batch -o local/SumatraPDF.cpp   ../sumatrapdf/src/SumatraPDF.cpp   < local/SumatraPDF.patch
patch --batch -o local/AppSettings.cpp  ../sumatrapdf/src/shared/AppSettings.cpp < local/AppSettings.patch
patch --batch -o local/Settings.cpp     ../sumatrapdf/src/Settings.cpp     < local/Settings.patch
patch --batch -o local/Settings.h       ../sumatrapdf/src/Settings.h       < local/Settings_h.patch
patch --batch -o local/RenderCache.cpp  ../sumatrapdf/src/shared/RenderCache.cpp < local/RenderCache.patch
patch --batch -o local/RenderCache.h    ../sumatrapdf/src/shared/RenderCache.h   < local/RenderCache_h.patch
patch --batch -o local/EngineMupdf.cpp  ../sumatrapdf/src/shared/EngineMupdf.cpp < local/EngineMupdf.patch
# 书签（Ctrl+B）改造
patch --batch -o local/TableOfContents.cpp       ../sumatrapdf/src/TableOfContents.cpp          < local/TableOfContents.patch
patch --batch -o local/AddFavoriteDialog.cpp      ../sumatrapdf/src/AddFavoriteDialog.cpp         < local/AddFavoriteDialog.patch
patch --batch -o local/SidebarPanel.cpp          ../sumatrapdf/src/SidebarPanel.cpp             < local/SidebarPanel.patch
patch --batch -o local/Menu.cpp                  ../sumatrapdf/src/Menu.cpp                     < local/Menu.patch
patch --batch -o local/shared/FavoritesCommon.cpp ../sumatrapdf/src/shared/FavoritesCommon.cpp  < local/FavoritesCommon.patch
patch --batch -o local/shared/MenuDefs.cpp       ../sumatrapdf/src/shared/MenuDefs.cpp          < local/MenuDefs.patch
patch --batch -o local/shared/Commands.cpp       ../sumatrapdf/src/shared/Commands.cpp          < local/Commands.patch
```

每个补丁都做过“应用后与 `local/` 副本逐字节比较”。

### 7.1 以后改一个功能的标准流程

```sh
cd /cluster/home/it_stu171/rea/sumatrapdf-build

# 1) 首次：从上游生成本地副本（以 Xxx.cpp 为例）
patch --batch -o local/Xxx.cpp ../sumatrapdf/src/Xxx.cpp < local/Xxx.patch

# 2) 编辑 local/Xxx.cpp

# 3) 增量构建（只重编受影响文件 + 链接）
bash build-static.sh

# 4) 冒烟：命令行渲染 / 无头 GUI
WINEPREFIX=/tmp/spw-pfx DISPLAY=:97 wine tree/out/rel64/SumatraPDF-static.exe \
    -log-to-file 'Z:\\tmp\\sp.log' 'Z:\\tmp\\t1.pdf'

# 5) 回写补丁
diff -u ../sumatrapdf/src/Xxx.cpp local/Xxx.cpp > local/Xxx.patch
```

注意：新增源文件或改 `premake5.lua` 后，必须先重跑 Premake 与 `adapt-ninja.py`，否则 Ninja 图里没有它。

## 8. 已知限制与未验证

- Windows 11 真机只人工确认“能用”，Direct2D 崩溃根因未取证（方法：`-log-to-file` 看 `InitDirect2D`、`%LOCALAPPDATA%\SumatraPDF-data\<sha1前6位>\crashinfo\*.dmp`、WER）。
- 恢复中断时本次运行不再保存设置（保守策略，可读日志确认）。
- 缓存预算 256 MiB 在普通文档上看不出差别；64 MiB 更省但回看会重渲染。
- MuPDF 64 MiB store 未显示收益，且未测大文档/图片密集型文档。
- Wine 下的内存数字不能等同于 Windows 原生。

## 9. 附录：文件索引

| 文件 | 作用 |
| --- | --- |
| `build-static.sh` | 设置 INCLUDE/LIB、库别名、调用 Ninja |
| `adapt-ninja.py` | 转换 Premake 生成的 Ninja 图 |
| `pack-static.py` | 打包字体/翻译/JS 为内嵌 PAK |
| `translations/` | 中文翻译快照与转换器 |
| `local/` | 全部本地源码副本与补丁 |
| `tree/` | 隔离构建视图（`.work` 中间产物、`out` 产物）|
| `README-zh.md` | 构建步骤与各功能说明 |
| `讲义-Linux交叉编译Windows程序.md` | 交叉编译原理、CMake 对比、XChange 分析 |
| `pdf-bookmarks/` | 内嵌的 PDF 书签脚本、批量清单、生成器与说明（见 §10）|
| 本文 | 架构、修改、编译的总览 |

## 10. 内嵌真正的 PDF 书签（资源层改造，不动 C++）

**Bookmark ≠ Favorite**：`Ctrl+B` 的 Favorites 只写 `SumatraPDF-settings.txt`；这里写的是 PDF 自身的 `/Outlines` 字典，F12 的 Bookmarks 侧栏与其他阅读器都能看到。

| 项 | 内容 |
| --- | --- |
| 依据 | 本 EXE 已静态链接 MuPDF 1.28.5 的 JS 运行器 `murun_main`（`src/SumatraPDF.cpp:16166, 16201`），`run` 与 `CmdRunPDFScript` 都可用；`Document.outlineIterator()` 支持 insert/delete，`doc.save()` 写回 PDF |
| 缺口 | 上游 `scripts.js` 来自私有 `sumatra-website` 仓库，本构建没有它，所以「Run PDF Script...」的 Example Scripts 原本是空的（见 `README-zh.md` 第 12 行）|
| 改动 | 新增 `pdf-bookmarks/` 三个 JS（单条书签 / 批量清单 / 只读列出），`make-scripts.py` 生成 `tree/.work/scripts.js`（10,967 字节，按 `// ---` 分割、`// Info:` 命名），`pack-static.py` 将其打进 `embedded-static.lzsa`；C++ 源码与 `local/` 补丁均未改 |
| 构建 | `python3 pdf-bookmarks/make-scripts.py` → `bash build-static.sh`；Ninja 只跑 3 步（重打资源包、编译 .res、重链），无需 Premake / `adapt-ninja.py` |
| 入口 | GUI：打开 PDF → `Ctrl+K` 搜 `Run PDF Script`（或右键 → Document → Run PDF Script...）→ Example Scripts → 选书签脚本 → 改脚本顶部 `CFG_*` 几行 → `Ctrl+Enter`；CLI：`SumatraPDF-static.exe run add-one-bookmark.js in.pdf out.pdf 42 "标题" append` |
| 为什么没有 `// Arg:` 参数行 | **Wine 6.0.2 下该对话框只要创建参数行就崩溃**（`EM_SETCUEBANNER` 路径，异常地址在 Wine 模块内），用**未改动的原 EXE** 点「Add argument」可稳定复现，属上游对话框在 Wine 下的既有缺陷，不是本次引入。故打包时默认剥掉 `// Arg:` 行，改由脚本顶部 CONFIG 块传参；`make-scripts.py --keep-arg-rows` 可恢复预填参数行（仅限真机 Windows）|
| 安全 | 输出恒为新文件，不覆盖输入；不触碰设置文件；签名 PDF 重写后签名失效 |
| 证据 | 200 页测试 PDF 写入 9 条（含 3 级嵌套），`dump` 页码全部正确；字节级校验目录含 `/Outlines`、9 个 `/Title` + 9 个 `/Dest`；`append` 模式 1→2 条、9→18 条；无参 CONFIG 路径与有参 CLI 路径都写出书签；`runtime-smoke.sh` PASS；资源包中 `scripts.js` 条目大小 10,967 / CRC32 与生成文件一致；GUI 实测对话框正常显示内嵌脚本且无崩溃（对照：含参数行版本必崩）|
| 已知差异 | Wine 6.0.2 CRT 不支持 MuPDF 新建输出用的 `fopen("wb+x")`（`ext/mupdf/source/fitz/output.c:392`），脚本自动回退 `incremental`；真机 Windows 11 走 `compress` 全量重写。真机 GUI 未取证 |
| 详细文档 | `pdf-bookmarks/README.md` |
