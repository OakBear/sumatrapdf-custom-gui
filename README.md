# SumatraPDF 定制构建 · Briss 式空白裁切

> **本仓库是 [SumatraPDF](https://github.com/sumatrapdfreader/sumatrapdf) 的衍生修改版（fork/衍生作品），
> 基于上游开源项目构建。原作者与贡献者见 [AUTHORS](https://github.com/sumatrapdfreader/sumatrapdf/blob/master/AUTHORS)，
> 项目主页 <https://www.sumatrapdfreader.org/>。**
>
> 我们只是在其之上叠加了自己的 GUI 改造，**核心阅读器与全部原版功能都属于上游项目及其作者**。
> 向 Krzysztof Kowalczyk 及所有 SumatraPDF 贡献者致谢与敬意。

---

## 这是什么

在 SumatraPDF 之上做的一批 GUI 体验改造，重点是**模仿 PDF-XChange 的标签栏/滚动**与
**Briss 式的空白自动裁切**。改造方式是把改动放在 `local/` 覆盖层（整文件副本 + 统一 diff 补丁），
由 `adapt-ninja.py` 映射进隔离的 `tree/` 构建视图，**上游源码目录始终保持只读、不改动**，
因此每一处改动都能以补丁形式审阅与复现。

## 相对上游的主要改动

- **Briss 式空白裁切**：分析全篇/检测当前页，自动识别页面有效内容区并裁掉四周空白；
  预览保持 Briss 风格（被裁区域半透明显示，便于核对）。
- **裁切结果持久化**：分析出的每页裁剪矩形按页宽高比例存入该文档的 FileState（`PageCrops`），
  重开文档自动套用、**无需重新分析**；PDF 文件本身不改动。页数变化则自动失效重算。
- **原版 / 裁剪视图切换**：命令面板 `Ctrl+K` 搜 *Original*，在整页原版与已保存的裁剪之间即时切换。
- **标签栏**：窄标签、横向滚轮切换、标签全路径提示、右侧下拉标签列表。
- **书签**：Ctrl+B 加书签，重建书签树不再整页闪白。

详见 [SUMATRA-改造总览.md](SUMATRA-改造总览.md)。

## 构建

本仓库只包含 **Linux 交叉编译脚手架 + 本地覆盖层 + 文档**，不含上游源码本体。
复现步骤、工具链要求见 [README-zh.md](README-zh.md)，上游基线提交记录在 [UPSTREAM.txt](UPSTREAM.txt)。

```sh
# 将 local/*.patch 应用到上游树后，用隔离视图构建
bash build-static.sh
```

## 下载

预编译产物见 [Releases](https://github.com/OakBear/sumatrapdf-custom-gui/releases)，
校验值见 [dist/SHA256SUMS.txt](dist/SHA256SUMS.txt)。

## 许可证与署名

- 上游 SumatraPDF 以 **GNU GPLv3** 发布，本衍生作品**同样以 GPLv3 发布**，完整许可证见 [COPYING](COPYING)。
- 原始版权归 SumatraPDF 项目及其作者所有；本仓库仅包含对其的修改部分，修改部分同样接受 GPLv3 约束。
- 第三方组件许可（MuPDF、UnRAR、HEIC、Fugue Icons 等）见上游 [AUTHORS](https://github.com/sumatrapdfreader/sumatrapdf/blob/master/AUTHORS) 与 [COPYING.BSD](COPYING.BSD)。
- Briss 的空白检测思路参考自 [Briss](https://github.com/mbaeuerle/Briss-2.0)（GPLv3），本实现为独立重写，未复制其代码。

## 免责声明

这是**非官方**修改版，与 SumatraPDF 原作者无关；如遇问题请勿向 SumatraPDF 上游报告。
