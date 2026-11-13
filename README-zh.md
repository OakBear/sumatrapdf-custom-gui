# SumatraPDF Linux 交叉构建（实验中）

目标：在 Linux x86-64 上构建 Windows 11 x64 便携版。静态程序已链接，完整程序运行效果尚未验证。

- 官方源码：`../sumatrapdf`，提交 `075b05ce7cfe3fd0ce1eec9cdff2ae8ea00c6e53`。
- 工具链：`../sumatrapdf-toolchain`（LLVM、Ninja、NASM、Microsoft CRT 和 Windows SDK）。工具和 SDK 各自的许可适用；Windows SDK/CRT 许可见 https://go.microsoft.com/fwlink/?LinkId=2086102 。
- `tree` 是隔离构建视图，`.work` 和 `out` 不写入官方源码目录。
- `adapt-ninja.py` 是仍在调试的构建图转换脚本；Ninja 预演成功不表示构建成功。
- `local/` 包含独立源代码副本及其统一 diff 补丁。UnRAR 的 SSSE3 函数仅在调用方检测 CPU 支持后执行；HEIC 的两个 Clang SIMD 路径和 MuPDF skew/deskew 改用原有标量实现，像素效果尚需验证，性能可能下降。
- `pack-static.py` 在隔离目录中使用 Wine 和上游 MakeLZSA 打包字体、JavaScript 库及翻译。
- `translations/` 保存从官方 AppTranslator 中文页面只读获取的快照和离线转换器。生成的翻译仅包含简体和繁体中文；其他语言回退为英文，运行时效果仍须验证。转换器修正了字符数量提示和书签提示中的参数含义，原始快照保留不变。
- 当前没有上游私有 `sumatra-website` 仓库，因此应用内离线手册和 PDF 脚本示例没有打包；上游 CI 也允许不包含它们。基本 PDF 阅读功能不依赖这些可选内容。

已验证：Wine 中运行上游 Premake 和 MakeLZSA；生成并校验包含字体和中文翻译的资源档案；生成 Windows x64 COFF 汇编对象；编译链接并通过 Wine 执行最小 Windows x64 程序；编译项目内一个 zlib C 文件。SDK 10.0.26100 和 CRT 14.44 已下载；由于该 CRT 的 C++ 标准库要求 Clang 19 或更新，已另外安装 LLVM 19，并在 Wine 中成功运行使用静态 CRT 和 C++ 标准库的测试程序。`bash build-static.sh` 已成功链接 `tree/out/rel64/SumatraPDF-static.exe`（退出码 0，最多 4 个并发任务）；脚本自动在隔离目录生成大小写敏感的 SDK 库别名。PE 头确认 AMD64 Windows GUI，未发现动态 VC 运行库依赖；可执行文件内嵌档案与构建档案字节一致。Wine 中 PDF 第 1 页渲染命令和第 999 页无效页码负例通过（`bash runtime-smoke.sh`）。上游 `-render` 测试默认把百分比 100 当作引擎的 100 倍缩放，脚本传入 `-zoom 1`；该测试不验证渲染像素。Wine 6.0.2/Xvfb 图形界面测试能够创建窗口并识别两页，但产生执行访问异常 minidump（0xc0000005），截图全黑；独立便携设置复测仍崩溃。同一环境的官方 3.6.1 便携版能显示 PDF，截图 OCR 可识别正文且运行 20 秒未见崩溃；自编译源码版本标记为 3.7，版本和构建均不同。测试环境亦报告软件图形驱动加载失败。单次 Wine 启动通过 `WINEDLLOVERRIDES="mscoree,mshtml,d2d1="` 禁用 Direct2D 后，应用使用现有后备路径，加载两页，20 秒内未见新崩溃；非黑截图的 OCR 识别出 NAME 等 PDF 标题片段，支持页面内容已显示，但 OCR 效果较差且无法目视检查。对照支持异常与 Wine 的 Direct2D 路径有关，具体原因尚未确定。Windows 11 原生运行和其他功能尚未验证。

## GDI+ 兼容性版本（Windows 启动问题）

用户报告原 Direct2D 默认版本在 Windows 11 上无法打开，只生成了设置文件。当前 `tree/out/rel64/SumatraPDF-static.exe` 已改为默认使用项目现有的 GDI+ 后备绘制路径，构建完成；该改动保存在 `local/Gfx.cpp` 和 `local/Gfx.patch`，由 `adapt-ninja.py` 指定，官方源码保持不变。此前 Direct2D 默认版本保存在 `tree/out/rel64/SumatraPDF-static-direct2d-original.exe`。新版本在 Wine 6.0.2 默认 Direct2D 可用配置下加载两页 PDF，20 秒未出现新崩溃，非黑截图 OCR 识别出部分页面标题；页面未能人工目视检查。命令行渲染和无效页码测试也通过。新版本仍需 Windows 原生复测，上述旧版 Wine 对照结果属于原版。

如需从上游文件重建本地副本，先备份现有本地修改，再执行：

```sh
patch --batch -o local/Gfx.cpp tree/src/gui/Gfx.cpp < local/Gfx.patch
```

## 从当前隔离构建树恢复构建

以下命令以当前布局和现有文件为前提；不会自动下载源码或工具。源码树须保持上文所列提交及其完整依赖，尤其是 `ext/`、字体和 `bin/MakeLZSA.exe`。需要 Linux x86-64、Python 3、Bash、GNU patch、Wine（含可用的 Wine 前缀），以及相邻工具链中的 LLVM 19、Ninja、NASM、Microsoft CRT 和 Windows SDK；版本和来源见相邻 `sumatrapdf-toolchain/TOOLCHAIN-INFO.md`。运行环境的 `PATH` 必须能找到 `wine`。以下操作会更新隔离目录中的构建产物和日志。

已有 `tree/.work/linux` 图和 `local/` 源副本时，直接增量构建：

```sh
cd /cluster/home/it_stu171/rea/sumatrapdf-build
bash build-static.sh
```

脚本限制 Ninja 为 `-j4`，创建三个大小写敏感的 SDK 库别名，并追加 `build-static.log`。资源规则通过 `pack-static.py` 调用 Wine 下的上游 MakeLZSA；它读取 `tree/.work/translations.txt`、可选的 `tree/.work/docs` 和 `scripts.js`、上游 JavaScript 及字体。若翻译文件缺失，脚本会生成空翻译；如需保留当前中文资源，请先执行下述转换器。默认嵌入字体；环境变量 `SUMATRA_NO_EMBED_FONTS=1` 会改变产物。

如需从上游对应文件重新生成四个本地副本，在构建目录执行以下命令。输出只写 `local/`，执行前应备份或检查已有本地修改：

```sh
patch --batch -o local/unrar.cpp tree/ext/a-unrar/unrar.cpp < local/unrar.patch
patch --batch -o local/heic.c tree/ext/heicdec/heic.c < local/heic.patch
patch --batch -o local/libwebp.c tree/ext/a-libwebp/libwebp.c < local/libwebp.patch
patch --batch -o local/ReadAloud_win.cpp tree/src/shared/ReadAloud_win.cpp < local/ReadAloud_win.patch
```

以上四个补丁曾分别应用到临时输出，并与现有副本逐字节比较。ReadAloud 补丁仅在包含 WinRT 头时局部改名以处理 clang-cl 的 `Color` 冲突。

## 重新生成 Ninja 图

保留现有隔离树的符号链接、源码依赖、四个本地副本和上述工具。必须在 `tree` 中运行 Premake，使其写入隔离的 `tree/.work/ninja`；不要在上游源码目录运行。然后生成中文翻译、转换构建图并构建：

```sh
cd /cluster/home/it_stu171/rea/sumatrapdf-build
(cd tree && WINEPREFIX="$(pwd)/../wine" WINEDEBUG=-all wine bin/premake5.exe ninja)
test -f tree/.work/translations.txt || python3 translations/convert_snapshots.py
python3 adapt-ninja.py
bash build-static.sh
```

Premake 命令使用现有上游 `bin/premake5.exe` 及 `tree/premake5.lua`；翻译转换器读取 `translations/` 内已保存的 cn/tw HTML 快照，不访问网络。转换器以独占创建方式写入，所以上述命令只在翻译文件缺失时运行。若要强制重新生成，先将现有 `tree/.work/translations.txt` 备份到其他路径，再移走原文件，然后运行 `python3 translations/convert_snapshots.py`；请保留备份以便恢复。转换图依赖原始 `tree/.work/ninja/*.ninja`、`local/` 源副本、`adapt-resource.py`、`pack-static.py` 和相邻工具链。上列完整重生序列是根据现有脚本和布局整理，未在本次文档更新中重新执行；本次成功链接使用既有 Premake 图。转换图后续更改应再次运行 `adapt-ninja.py`。

## 路径和迁移限制

当前 `tree/bin`、`cmd`、`ext`、`src` 和两个 Premake 文件是指向 `/cluster/home/it_stu171/rea/sumatrapdf` 的绝对符号链接；生成的 Linux Ninja 图也嵌入构建目录和工具链绝对路径。移动或复制目录后，须先重建这些符号链接，确保源码、构建目录和 `sumatrapdf-toolchain` 保持脚本要求的相邻布局，再重新运行 Premake 和 `adapt-ninja.py`；不要沿用旧图或假定已有对象跨路径有效。Wine 前缀也可能包含绝对路径，迁移后需检查或重新初始化。资源脚本直接从构建目录的相邻 `sumatrapdf` 读取输入，而非仅依赖 `tree`。此流程未证明不同主机或迁移后可直接复现相同字节的二进制。

## 会话保存与多窗口恢复（本地实验）

`local/SumatraPDF.cpp`、`local/AppSettings.cpp`、`local/Settings.h` 和 `local/Settings.cpp` 是在上游源码副本上叠加的本地改动，官方源码目录不变；构建图按路径覆盖到 `tree/src`。三项改动：

- **定时快照**：消息循环用 `SetTimer(nullptr, 0, 3*60*1000, ...)` 建立线程定时器，每 3 分钟在空闲时调用 `SaveSettingsNow()`；非正常关闭时最多丢失约 3 分钟内的会话变化。定时器受 `gIsStartup`、`gDontSaveSettings` 和 `Perm::SavePreferences` 约束，且不属于任何窗口，关闭某个窗口不会取消它。
- **最后打开时间**：`FileState` 新增 `lastOpenedUtc`（`Settings.cpp` 字段表同步为 37 项），在成功打开非临时 PDF、切换标签页、恢复会话时写入 UTC 时间。仅记录“用户真正查看过”的文档；恢复会话时只更新每个窗口的当前标签页，历史中的其他文档保持原值。
- **多窗口恢复**：恢复期间设置 `gRestoringSessionSnapshot`，暂停所有设置写入，避免半恢复状态覆盖磁盘上的会话；若恢复被中断（会话代数变化或窗口被关闭），该进程后续不再写设置，这是有意的保守策略，代价是本次运行不会保存新会话。全部窗口恢复成功后解除并立即安排一次保存。
- **原子写入的回退**：设置改用同目录临时文件写入后重命名发布；`GetTempFileNameW` 受 `MAX_PATH` 限制，路径过长时回退为原来的就地写入，并在日志记录该回退，避免长配置目录下保存静默失效。

验证（Wine 6.0.2、Xvfb、隔离子前缀、`-appdata` 独立目录）：打开两个 PDF 各占一个窗口，用 Ctrl+Q 退出后，设置文件同时包含两个窗口的 `SessionData` 条目和两条 `LastOpenedUtc`；重启后恢复出两个窗口（标题分别为两个文件）。把定时器临时改为 10 秒重建测试版本，36 秒内日志出现 3 次 “session snapshot timer fired”，每次伴随 `SaveSettings`，说明定时器确实在消息循环中触发；正式版本已恢复 3 分钟周期。Windows 原生环境、异常终止（断电/强杀）和窗口级恢复失败路径尚未测试。

## MuPDF 缓存上限对照测量（未显示收益）

上游每个文档引擎各自创建 `fz_new_context(..., FZ_STORE_DEFAULT)`（256 MiB），这是软性逐出预算而非预分配。`local/EngineMupdf.cpp` 把该预算改为 64 MiB。为判断它能否降低多文档场景的常驻内存，构建了两个仅此常量不同的可执行文件（其余改动完全相同），在同一 Wine 前缀中同时打开 5 个小型 PDF，等待 28 秒后统计进程 RSS，各测两轮：

| 版本 | 第 1 轮 RSS | 第 2 轮 RSS |
| --- | --- | --- |
| 256 MiB（上游默认） | 130,964 KB | 132,096 KB |
| 64 MiB（本地实验） | 132,252 KB | 132,300 KB |

差异在噪声范围内，且 64 MiB 版本没有更低。原因是这些文档很小，256 MiB 预算从未接近耗尽，缩小预算不改变已加载对象数量；该实验也不能外推到大型或图片密集型文档，未测页切换延迟和解码开销。结论：仅调小 MuPDF store 不是实现“多个 PDF 只占一个的内存”的有效手段，与先前对 PDF-XChange 的分析一致——它的低占用来自按文档增删缓存而非更小的统一上限。当前 `tree/out/rel64/SumatraPDF-static.exe` 使用 64 MiB；是否保留该值应另行决定。

## 本地改动补丁与重建

`local/` 下每个改动文件都有对应统一补丁；**每个补丁都已逐个应用到上游原文并逐字节比较，结果与本地副本一致**。要从上游重新生成：

```sh
patch --batch -o local/SumatraPDF.cpp ../sumatrapdf/src/SumatraPDF.cpp < local/SumatraPDF.patch
patch --batch -o local/AppSettings.cpp ../sumatrapdf/src/shared/AppSettings.cpp < local/AppSettings.patch
patch --batch -o local/Settings.cpp ../sumatrapdf/src/Settings.cpp < local/Settings.patch
patch --batch -o local/Settings.h ../sumatrapdf/src/Settings.h < local/Settings_h.patch
patch --batch -o local/EngineMupdf.cpp ../sumatrapdf/src/shared/EngineMupdf.cpp < local/EngineMupdf.patch
# 书签（Ctrl+B）改造，2026-11-13 新增
patch --batch -o local/TableOfContents.cpp ../sumatrapdf/src/TableOfContents.cpp < local/TableOfContents.patch
patch --batch -o local/SidebarPanel.cpp ../sumatrapdf/src/SidebarPanel.cpp < local/SidebarPanel.patch
patch --batch -o local/Menu.cpp ../sumatrapdf/src/Menu.cpp < local/Menu.patch
patch --batch -o local/shared/FavoritesCommon.cpp ../sumatrapdf/src/shared/FavoritesCommon.cpp < local/FavoritesCommon.patch
patch --batch -o local/shared/MenuDefs.cpp ../sumatrapdf/src/shared/MenuDefs.cpp < local/MenuDefs.patch
patch --batch -o local/shared/Commands.cpp ../sumatrapdf/src/shared/Commands.cpp < local/Commands.patch
```

执行前先备份现有 `local/` 副本；`Settings.h` 的补丁单独命名为 `Settings_h.patch`，避免与 `Settings.cpp` 的补丁同名覆盖。

## 渲染缓存预算与空闲清理（借鉴 PDF-XChange 的实现）

`local/RenderCache.h` / `local/RenderCache.cpp` 在 `src/shared/` 上叠加了本地改动，补丁为 `local/RenderCache_h.patch` 与 `local/RenderCache.patch`。设计要点：

| 机制 | 实现 |
| --- | --- |
| 字节预算 | 缓存条目记录 `byteSize`，`RenderCache.cachedBytes` 随增删维护；上限 `CacheBudgetBytes()` = 256 MiB |
| 软上限 | 预算超了也**不会**逐出当前可见的瓦片；可见内容优先，宁可暂时超预算 |
| 淘汰顺序 | 同文档不可见页 → 旧缩放/旋转 → 屏幕外瓦片 → 其他文档中**最久未绘制**的条目 |
| 空闲清理 | 条目记录 `lastUse`（`GetTickCount64`），超过 `kCacheIdleTtlMs`（5 分钟）未绘制且页面不可见则丢弃 |
| 触发点 | `Add()` 里顺带清理；另外由 3 分钟的会话快照定时器调用 `TrimIdleEntries()`，否则空闲时无人调用 `Add()` |

安全性边界：只丢缓存条目，不接触文档、注释或未保存修改；`refs > 1` 的条目不丢；当前显示的页不丢；页面会按需重渲染。

实测（Wine 6.0.2、1400x1000、200 页测试文档、连续翻 120 页后测 RSS）：

| 版本 | RSS | 说明 |
| --- | --- | --- |
| 改动前 | 194 MB | 只有 256 条目上限 |
| 64 MiB 预算（实验） | 142 MB | 缓存稳定在 28 项 / 66 MB，日志显示持续淘汰 |
| 256 MiB 预算（正式） | 199 MB | 该负载工作集约 66 MB，没触到上限，故无变化 |

空闲清理（TTL 临时改为 20 秒的实验版）：翻页后 130 MB，静置 70 秒后 122 MB，日志为 `dropped 13 idle entries (29 MB)`。

结论：字节预算管的是**最坏情况**（深层缩放、超大页面、256 个瓦片接近 1 GB），TTL 管的是**忘记关掉的标签页**；常规小文档看不出差别。要更激进可把 `kBudget` 调小（64 MiB 已验证可行，代价是回看时重新渲染）。

未做的部分与原因：渲染线程本来就是**全局共享池**（`RenderCache` 是进程级单例，上限 `max(8, 核数)` 且 ≤32），不是每文档一条线程，因此不需要额外改造；位图池化和进程外转换收益不明、风险更高，暂不引入。


## 书签（Ctrl+B）语义改造：从「Favorites」到「Bookmarks」

### 改之前是什么

上游 SumatraPDF 里 **Ctrl+B 不是书签**：

| 快捷键 | 上游命令 | 实际行为 |
| --- | --- | --- |
| `Ctrl+B` | `CmdFavoriteAdd`「Add to favorites」 | 弹命名框，把「文件名 + 页码 + 滚动位置」写进 `SumatraPDF-settings.txt` 的 `Favorites` 段 |
| `F12` | `CmdToggleBookmarks`「Show Bookmarks」 | 打开 **PDF 自带的 /Outlines 目录树**，只读 |

而「Favorites」面板默认 `ShowFavorites = false`，所以用户按 Ctrl+B 存了东西，界面上几乎看不到任何反馈——这就是「Ctrl+B 加了 favourite 但不是 bookmark」的根源。SumatraPDF 没有 PDF 写出器，无法把条目写进 PDF 的 `/Outlines`。

### 改之后是什么

**把 Favorites 当作「用户书签」，在 F12 的 Bookmarks 面板里和 PDF 目录合并显示**：

| 行为 | 实现 |
| --- | --- |
| 菜单/命令改名 | `MenuDefs.cpp`「Add to favorites」→「Add Bookmark」，「Remove from favorites」→「Remove Bookmark」；`Commands.cpp` 命令名同步 |
| 书签显示在目录上方 | `TableOfContents.cpp` 新增 `BookmarksModel : TreeModel`，把当前文件的书签包装成 `TocItem`（`id >= kBookmarkItemIdBase`）排在 `/Outlines` 各项之前 |
| 行外观与目录一致 | 书签名 + 右侧页码（`TocItem::pageNo` 参与 `TocPageReserveDx` 的留白计算），本章式文档退回「名字（章节）」 |
| 点击跳转 | `TocTreeClick` 拦截书签行 → `JumpToFavorite()`，恢复页码 + 滚动位置 + 章节定位 |
| 右键删除 | 书签行右键只有「Remove Bookmark」→ `DelFavorite()` + `ScheduleSaveSettings()`；目录行仍是原来的目录菜单 |
| 无目录也能有书签 | `IsSidebarViewAvailable(Bookmarks)`、工具栏/右键菜单的 `HasToc()` 判断都放宽为 `HasToc() || HasUserBookmarks(win)` |
| 加完自动打开 | 第一个书签落盘后自动展开 Bookmarks 面板（受 `ShowToc` 控制），像 PDF-XChange 加书签那样 |
| 立即刷新 | `UpdateFavoritesTreeForAllWindows()` 里挂钩 `RefreshBookmarksForFavoritesChange()`，用 `uitask::Post` 延后重建树；重建时先 `ShowWindow(SW_HIDE)` 再 `SW_SHOW`，否则可见的原生 TreeView 会保留旧行（删掉的书签那行还在，整体上移一行） |

**零回归设计**：文件没有书签时 `BookmarksTreeModel()` 直接返回原来的 `TocTree`，模型、行数、菜单、快捷键全部与上游一致；`Otherwise` 只在真的有书签时才走合并模型。

### 边界与没做的事

* **PDF 文件本身不被修改**：书签仍然存在 `SumatraPDF-settings.txt`（每文件一个 `Favorites` 段）。要写进 PDF 的 `/Outlines`，走 `pdf-bookmarks/` 的 MuPDF JS 路线（见 `pdf-bookmarks/README.md`）。
* 目录过滤框（Search Bookmarks）输入关键字时，目前只过滤 `/Outlines`，书签行不参与过滤；`UpdateTocSelection` 也不给书签行打「当前页」高亮。
* 书签名沿用上游命名对话框，没有做重命名/拖拽排序。

### 验证（Wine 6.0.2 + Xvfb，全新 appdata）

| 场景 | 结果 |
| --- | --- |
| 无目录 PDF（200 页）Ctrl+B | 面板自动打开，出现一行 `beta 4` |
| 有目录 PDF（自制 6 页 3 条目录）Ctrl+B | 书签行在目录上方：`alpha 2 / Chapter 1 1 / Section 1.1 2 / Chapter 2 4` |
| 点书签行 | 从第 1 页跳到书签页（页码框 1 → 2） |
| 右键书签 → Remove Bookmark | 设置文件 `Favorites [ ]` 清空，行立即消失，目录恢复 3 行，无重影 |
| 无目录无书签 PDF 按 F12 | 与上游一致：不打开面板（`ShowToc = false`） |
| 该文件有目录但已删光书签 | 合并模型退化为纯 `TocTree`，行为等同上游 |

### 怎么加书签（三个入口，都实测可用）

| 入口 | 操作 | 结果 |
| --- | --- | --- |
| 快捷键 | 在页面上按 `Ctrl+B` | 弹「Name for page 2 (optional)」+ 按钮 `[Add Bookmark]` |
| 页面右键 | 页面上右键 → 「Add Bookmark (page 2)　Ctrl + B」 | 同上 |
| 书签面板右键 | 在 Bookmarks 面板空白处右键 → 「Add Bookmark (page 2)　Ctrl + B」 | 同上（面板里一行都没有时也能用） |
| 命令面板 | `Ctrl+K` → 输入 `bookmark` → **Add Bookmark** | 实测列出该命令，回车即弹命名框 |
| 菜单栏 | Bookmarks → Add Bookmark | 本构建下没有传统菜单栏（见下一节），`F9` 可尝试打开 |

四个入口最终都走同一个命令 `CmdFavoriteAdd`，所以命名框、页码、滚动位置的行为完全一致；加完立即出现在面板顶部（`ShowToc` 为真时面板自动打开）。

菜单/对话框文案统一为书签语义：`Add Bookmark` / `Remove Bookmark` / `Add Bookmark (page N)` / `Add Bookmark (chapter C, page P)`；原来的 `Favorites` 顶层菜单改名为 `Bookmarks`，其中「Show Favorites」改为「Show Bookmark List」。

### 一个必须记住的构建坑：`tree/src` 里的符号链接会写穿上游

`tree/src/AddFavoriteDialog.cpp` 原先是指向 `../sumatrapdf/src/AddFavoriteDialog.cpp` 的**符号链接**（只有被覆盖过的文件才是实体副本）。第一次 `cp local/AddFavoriteDialog.cpp tree/src/` 直接改写了上游克隆里的文件（`git status` 出现 ` M src/AddFavoriteDialog.cpp`）。

处理：`rm` 掉符号链接 → `cp` 实体副本 → `git -C ../sumatrapdf checkout -- src/AddFavoriteDialog.cpp` 还原上游 → 重新构建。
**以后覆盖任何新文件前先 `ls -l tree/src/X` 确认是不是符号链接。**

### 这次顺手去掉了 Favorite 的界面

| 位置 | 之前 | 现在 |
| --- | --- | --- |
| 侧栏视图按钮 | 书签 / 缩略图 / **星标（Favorites）** 三个 | 只剩书签 / 缩略图（星标按钮隐藏） |
| 文档右键 | 有 Show Favorites | 已移除 |
| Bookmarks 菜单 | Add / Remove / Show Favorites / Show Favorites in Tab | Add Bookmark / Remove Bookmark / Tab Groups |
| 命令面板 | Delete Favorite、Go to Next Favorite… | Delete Bookmark、Go to Next Bookmark… |

底层数据仍然复用 Sumatra 的 `Favorites` 段（那是它唯一能按文件存「名字 + 页码 + 滚动位置」的地方），但用户界面里已经看不到 "Favorite" 这个词。

### 书签（/Outlines）和 Favorite 到底是谁的东西

| | PDF 自带的书签 | SumatraPDF 的 Favorite |
| --- | --- | --- |
| 存储位置 | PDF 文件内部的 `/Outlines` 树 | `SumatraPDF-settings.txt` 的 `Favorites [ ... ]` 段 |
| 换台机器/换个阅读器 | 还在文档里 | 没了（只在本机这个软件的配置里） |
| 内容 | 标题 + 目标页/坐标 | 名字 + 页码 + 滚动位置（外加章节号） |
| Sumatra 能不能改 | **只能读**（没有 PDF 写出器） | 能读写 |

所以你在别的 PDF 里从没见过 "Favorite" —— 它根本不是 PDF 规范的一部分，是 SumatraPDF 自己的功能（PDF-XChange 里对应的是它的书签面板）。本仓库的做法是把这份「软件自己的记录」在界面上**完全按书签呈现**：`Ctrl+B` 加进去，立刻出现在 F12 的书签面板里，和 PDF 自带的 `/Outlines` 混排（自带的在下、你加的在上）。PDF 文件本身仍然不被修改。

如果要让书签真正写进 PDF 的 `/Outlines`，走 `pdf-bookmarks/` 里的 MuPDF JS 脚本（`add-pdf-bookmarks.js`），那是另一条路线，不进 GUI。

### Ctrl+B 加书签时整页闪一下（已修）

原因：重建书签树时会`ShowWindow(SW_HIDE)`→删除→重建→`SW_SHOW`，中间状态（空面板、半布局的画布）会被真实绘制出来，看起来就是整页闪白。

修法（`local/TableOfContents.cpp`）：用 `LockWindowUpdate(win->hwndFrame)` 把整个重建过程锁住（RAII 结构 `ScopedWindowUpdateLock`），结束后只做一次 `RDW_INVALIDATE|RDW_NOERASE` 的重绘，不擦背景。

### 关于「菜单栏默认隐藏」

本构建开了 `UseTabs`，SumatraPDF 会改用自定义标题栏，传统菜单栏（File/Edit/...）默认不显示 —— 这跟工具栏有没有空位无关，工具栏只是工具栏。

实测可用的入口：

| 入口 | 结果 |
| --- | --- |
| `Ctrl+K` 命令面板 | 输入 `bookmark` 会列出 **Add Bookmark　Ctrl + B**、Show Document Bookmarks、Sort Bookmarks By Name 等（截图验证过） |
| `F9` 或设置 `ShowMenubarWithTabs = true` | 尝试创建菜单栏 rebar；**在 Wine 下这条 rebar 不渲染**（Windows 上应正常，未实测） |
| `Ctrl+B` / 页面右键 / 面板空白处右键 | 直接加书签，不依赖任何菜单 |

本仓库把 `ShowMenubarWithTabs` 的默认值改成了 `true`（`local/Settings.cpp`），在 Windows 上打开就是带菜单栏的界面。

### 在 VNC 桌面上直接体验这个 EXE

EXE 是纯静态链接的 Windows 程序，Wine 直接跑，不需要 Windows，也不需要重新编译：

```sh
cd /cluster/home/it_stu171/rea/sumatrapdf-build
./run-on-desktop.sh                 # 起始页
./run-on-desktop.sh ~/doc/a.pdf     # 打开指定 PDF
```

脚本做的事：`DISPLAY${DISPLAY:-:0}`（本机 VNC 桌面就是 `:0`）、Wine 前缀放本地磁盘 `/tmp`、配置（含书签）放 `~/sumatra-bm-appdata`，所以关掉重开书签还在。

| 事项 | 说明 |
| --- | --- |
| 为什么前缀放 `/tmp` | BeeGFS 上的 `$HOME` 建前缀要几分钟，`/tmp` 几十秒 |
| 重启后 | `/tmp` 没了，脚本会自动重建前缀（几十秒），书签不受影响 |
| 已知显示问题 | Wine 下侧栏偶尔出现黑块（原版也一样），换页/改窗口大小会刷好 |
| 字体 | Wine 没有中文字体，书签名叫中文会显示成方块，配置文件里的内容是对的 |

### 版本管理（git）

本目录已经是独立 git 仓库（上游克隆不在其中，只记录 commit 号，见 `UPSTREAM.txt`）：

```sh
cd /cluster/home/it_stu171/rea/sumatrapdf-build
git log --oneline          # 历史
git tag                    # 可靠版本标签
git checkout v1.0-bookmark # 回到这一版（dist/ 里的 EXE 一起回来）
git diff v1.0-bookmark     # 之后改了什么
```

`tree/`（295 MB 构建树）、`wine/`、`*.lzsa`、日志都在 `.gitignore` 里；仓库只保存 `local/` 覆盖源码与补丁、脚本、文档和 `dist/` 里的 EXE。

### 补丁

```
local/TableOfContents.patch     490 行（合并模型、书签行右键菜单、面板空白处右键加书签、重建与刷新）
local/AddFavoriteDialog.patch    29 行（命名框标题与按钮 Add Favorite → Add Bookmark）
local/FavoritesCommon.patch      44 行（HasUserBookmarks、刷新挂钩、菜单动态文案改书签）
local/Menu.patch                 62 行（右键菜单文案与启用条件）
local/MenuDefs.patch             49 行（菜单文案与 Bookmarks 顶级菜单）
local/SidebarPanel.patch         23 行（Bookmarks 视图可用性放宽）
local/Commands.patch             11 行（命令名）
local/SumatraPDF.patch          151 行（累加：会话/缓存/书签声明）
```

八个补丁都用 `patch -p1` 在纯净上游副本上重放验证过，结果与 `local/` 副本**逐字节一致**。


产物：`dist/SumatraPDF-static-bookmarks.exe`，21,800,960 字节，
SHA-256 `c99676997d898f49b102c70bde2446532fc22178654087f8e790a0f56237c5d7`（2026-11-13 20:55，含上面的书签改造 + 防闪烁）。
