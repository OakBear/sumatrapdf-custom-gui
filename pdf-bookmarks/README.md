# 在这个 EXE 里写入「真正的 PDF 书签」

**Bookmark ≠ Favorite。** Favorites（`Ctrl+B`）只是写进 `SumatraPDF-settings.txt` 的阅读位置，换台电脑就没了、也不会出现在别的阅读器里。这里做的是写进 PDF 文件自身的 `/Outlines` 字典（PDF outline / TOC），F12 的 **Bookmarks** 侧栏、Adobe、PDF-XChange、浏览器都能看到。

## 原理（为什么不用改 C++ 源码）

这个单文件 EXE 里已经静态链接了 MuPDF 1.28.5 的 JS 运行器（`murun_main`）：

- GUI：`CmdRunPDFScript` → **Run PDF Script...** 对话框（右键 → Document → Run PDF Script...，或 `Ctrl+K` 搜 "Run PDF Script"）。
- CLI：`SumatraPDF-static.exe run script.js arg1 arg2 ...`（`src/SumatraPDF.cpp:16166, 16201`，`gToolNames` 含 `run`）。
- JS API：`Document.openDocument()`、`doc.outlineIterator()` 的 `insert/delete/update`、以及 `doc.save(path, "compress")` 会把 `/Outlines` 真正写回 PDF（`ext/mupdf/source/tools/murun.c:4391, 4508, 8130`）。
- 对话框里的脚本由**内嵌的 `scripts.js`** 提供（`PDFScriptCommon.cpp:22`），按 `// ---` 分割、用 `// Info:` 取名字。

上游靠私有的 `sumatra-website` 仓库生成 `scripts.js`，本构建没有它，所以原来「Example Scripts」列表是空的（见 `README-zh.md` 第 12 行）。本次把下面三个脚本打进了资源包，等同给 EXE 加了一个内置的书签功能。

> **注意**：这个对话框**只要创建参数行就会在 Wine 6.0.2 下崩溃**（用未改动的原 EXE 点「Add argument」可稳定复现，是既有缺陷）。因此打包版本特意**不含 `// Arg:` 行**，改用脚本顶部的 `CFG_*` 配置块传参。详见「已知限制」。

## 文件

| 文件 | 作用 |
| --- | --- |
| `add-one-bookmark.js` | 加**一条**书签：填页码（1 起）+ 标题即可，最常用 |
| `add-pdf-bookmarks.js` | 从 `bookmarks.txt` **批量**导入，支持多级嵌套、`replace` / `append` |
| `dump-pdf-bookmarks.js` | 只读列出某 PDF 的书签，用来验证结果 |
| `bookmarks.txt` | 批量书签示例 / 模板 |
| `make-scripts.py` | 把以上三个脚本打成 `tree/.work/scripts.js`（资源包输入）；默认剥掉 `// Arg:` 行 |
| `../tree/out/rel64/SumatraPDF-static.exe` | **已重建**，内嵌这三个脚本；SHA-256 `3a626d1155f0c0318ea404d8f8f0a4cc7a185c3072796f35ad39010c56f14c8b` |
| `../tree/out/rel64/SumatraPDF-static-no-bookmark-scripts.exe` | 重建前的原 EXE 备份 |

## 用法 A：GUI（不敲命令行）

1. 用 `SumatraPDF-static.exe` 打开一个 PDF（该命令只在当前标签是 PDF 时可用）。
2. `Ctrl+K` → 输入 `Run PDF Script` → 回车；或右键 → **Document** → **Run PDF Script...**。
3. 点 **Example Scripts**，列表里现在有 3 项；选 **Add one PDF bookmark (real outline entry) at a given page.**
4. 编辑器里改脚本顶部的 `CFG_*` 几行（**Windows 路径要写双反斜杠**）：
   ```js
   var CFG_INPUT  = "Z:\\books\\book.pdf";        // 当前 PDF
   var CFG_OUTPUT = "Z:\\books\\book-out.pdf";    // 新文件，别和输入同名
   var CFG_PAGE   = 42;                           // 1 起
   var CFG_TITLE  = "Chapter 42";
   var CFG_MODE   = "append";                     // append 保留旧书签 / replace 清空重建
   ```
5. `Ctrl+Enter` / **Run Script**。下方输出窗显示 `OK: 'Chapter 42' -> page 42`。
6. 打开生成的 PDF，`F12` 即可在 Bookmarks 侧栏看到。

批量导入：改选 **Add real PDF bookmarks (outline / TOC) from a text list.**，多一个 `CFG_LIST = "Z:\\books\\bookmarks.txt"`，把 `bookmarks.txt` 的章节表改成自己的即可；验证用第三项 **List the PDF bookmarks**，改 `CFG_INPUT` 指向生成的文件。

> 命令行有参数时（`run script.js in.pdf out.pdf ...`）参数优先，`CFG_*` 会被忽略，两种入口互不冲突。

## 用法 B：命令行（可脚本化 / 批量）

```bat
SumatraPDF-static.exe run add-one-bookmark.js in.pdf out.pdf 42 "Chapter 42" append
SumatraPDF-static.exe run add-pdf-bookmarks.js in.pdf out.pdf bookmarks.txt replace auto
SumatraPDF-static.exe run dump-pdf-bookmarks.js out.pdf
```

Linux/Wine 下路径用 `Z:\...` 形式。`scriptArgs[0..]` 就是命令行参数；不带参数时脚本读顶部的 `CFG_*`（GUI 用法）。

## bookmarks.txt 格式

```
# level,page,title   （页号从 1 开始；# 开头和空行忽略）
1,1,Chapter 1 Introduction
2,3,1.1 Background
2,7,1.2 Method
1,25,Chapter 2 Portfolio Construction
2,30,2.1 Constraints
3,33,2.1.1 Turnover limits
1,60,Chapter 3 Risk Models
```

`level` 1 = 顶层，2 挂在最近一个 level 1 下，3 挂在最近一个 level 2 下。标题里可以有逗号（只按前两个逗号切分）。页码超出总页数会被夹到最后一页并打印警告。

## 实测（Wine 6.0.2，200 页 `/tmp/many.pdf`）

| 项 | 结果 |
| --- | --- |
| 批量导入 9 条（含 3 级嵌套） | 输出 238,993 字节；`dump` 显示页码 1/3/7/25/30/33/60/120/180 全部正确 |
| 独立字节校验 | 目录含 `/Outlines 404 0 R`，9 个 `/Title` + 9 个 `/Dest`，标题字符串可直接搜到 |
| `append` 模式 | 单条追加 1→2 条；批量追加 9→18 条，原有书签不丢 |
| 无参 CONFIG 路径 | 不带任何 `scriptArgs` 运行，同样写出书签（模拟 GUI 改完配置后按 Ctrl+Enter） |
| GUI 对话框 | Xvfb 实测正常显示内嵌脚本、无崩溃；对照含 `// Arg:` 行版本必崩 |
| `runtime-smoke.sh` | PASS（第 1 页渲染 + 999 页负例） |
| `info` 工具 | 输出 PDF 仍为 200 页 |
| 资源包校验 | `scripts.js` 条目 10,967 字节、CRC32 与生成文件一致 |

## 已知限制 / 注意

- **Wine 6.0.2 的 CRT 不支持 `fopen("wb+x")`**（MuPDF 新建输出用的模式，`ext/mupdf/source/fitz/output.c:392`），会报 `Invalid argument`。脚本已做自动回退：先试 `compress` 全量重写，失败则改 `incremental` 增量保存。真机 Windows 11 走 `compress`，产物更干净；增量版会保留修订历史、文件略大。
- **Wine 6.0.2 下 `Run PDF Script` 对话框一旦创建参数行就崩溃**：用**未改动的原 EXE** 打开该对话框、点「Add argument」也能稳定复现（异常地址在 Wine 模块内，非本 EXE 代码），属既有缺陷。因此 `make-scripts.py` 默认剥掉 `// Arg:` 行、改用 `CFG_*` 块。若你只在真机 Windows 上运行、想要自动预填的输入/输出路径，可 `python3 make-scripts.py --keep-arg-rows && bash ../build-static.sh`；**该版本在 Wine 里打开对话框会崩**。
- 输出永远是**新文件**，不会覆盖输入；脚本不会去动 `SumatraPDF-settings.txt`。
- 书签写入 PDF 文件本身 → 会改变文件哈希；**已签名的 PDF 重写后签名失效**。加密 PDF 需要密码，脚本未处理。
- JS 运行器是独立子进程（`StartPDFScript` 调自身 `run`，`PDFScriptCommon.cpp:276`），对话框崩溃不影响主窗口。
- 重建 EXE 会改变 PE `TimeDateStamp`，因此 SHA-256 会变；总览文档 §0 已更新，备份见上表。
- 若以后改了脚本：改 `pdf-bookmarks/*.js` → `python3 make-scripts.py` → `bash ../build-static.sh`（只重打资源包 + 重链，约 3 步）。不需要重跑 Premake，也不需要改 C++。
