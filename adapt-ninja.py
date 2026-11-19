#!/usr/bin/env python3
"""Adapt the isolated Premake graph for Linux-hosted LLVM (experimental)."""
from pathlib import Path
import os
import re
import shutil

root = Path(__file__).resolve().parent
raw = root / "tree/.work/ninja"
dst = root / "tree/.work/linux"
dst.mkdir(parents=True, exist_ok=True)
for graph in raw.glob("*.ninja"):
    shutil.copy2(graph, dst / graph.name)
tools = root.parent / "sumatrapdf-toolchain"
for path in dst.glob("*.ninja"):
    s = path.read_text()
    if path.name == "build.ninja":
        s = re.sub(r"(?m)^default .+$", "default SumatraPDF-static_Release_x64", s)
    s = re.sub(r"\$+\(", "$$（", s).replace("$$（", "$$(")
    s = s.replace("command = cl $cflags", f"command = {tools}/llvm19/bin/clang-cl --no-default-config --target=x86_64-pc-windows-msvc $cflags")
    s = s.replace("command = cl $cxxflags", f"command = {tools}/llvm19/bin/clang-cl --no-default-config --target=x86_64-pc-windows-msvc $cxxflags")
    s = s.replace("command = cl /nologo", f"command = {tools}/llvm19/bin/clang-cl --no-default-config --target=x86_64-pc-windows-msvc /nologo")
    s = s.replace("command = cl $in $links /link", f"command = {tools}/llvm19/bin/lld-link $in $links")
    s = s.replace("command = lib $in", f"command = {tools}/llvm19/bin/llvm-lib $in")
    s = s.replace("command = rc /nologo /fo$out $in $resflags", f"command = {tools}/llvm19/bin/llvm-rc /nologo /fo$out $resflags $in")
    s = s.replace("command = copy /B /Y $in $out", "command = cp $in $out")
    s = s.replace(" /c $in\n  description = Generating precompiled", " /showIncludes /c $in\n  deps = msvc\n  description = Generating precompiled")
    s = re.sub(r"(?<!\S)/(?:GL|MP|WX)(?=\s|$)", "", s)
    s = re.sub(r"^(c(?:xx)?flags_[^\n]+)$", r'\1 /D"UNICODE" /D"_UNICODE"', s, flags=re.M)
    s = re.sub(r'  customcommand = cmd /C "\.\.\\\.\.\\bin\\nasm\.exe (.*)"', lambda m: f"  customcommand = {tools}/nasm/bin/nasm " + m.group(1).replace('\\"', '"'), s)
    if path.name == "heicdec.ninja":
        s = s.replace("../../ext/heicdec/heic.c", str(root / "local/heic.c"))
    if path.name == "a-unrar.ninja":
        s = s.replace("../../ext/a-unrar/unrar.cpp", str(root / "local/unrar.cpp"))
    if path.name == "a-libwebp.ninja":
        s = s.replace("../../ext/a-libwebp/libwebp.c", str(root / "local/libwebp.c"))
    if path.name == "tesseract.ninja":
        for name, flag in {"dotproductavx": "/arch:AVX", "dotproductfma": "-mavx -mfma", "intsimdmatrixavx2": "-mavx2 -mno-fma", "dotproductsse": "-msse4.1", "intsimdmatrixsse": "-msse4.1"}.items():
            pattern = r"(build [^\n]*/" + name + r"\.obj: cxx_msc-v145 [^\n]*\n  cxxflags = [^\n]*)"
            s = re.sub(pattern, lambda m: m.group(1) + " " + flag, s)
    if path.name == "mupdf.ninja":
        # Skew and deskew use SSE without runtime dispatch; keep scalar paths.
        pattern = r"(build [^\n]*/(?:deskew|skew)\.obj: cc_msc-v145 [^\n]*\n  cflags = [^\n]*)"
        s, count = re.subn(pattern, lambda m: m.group(1) + " /DARCH_HAS_SSE=0", s)
        assert count > 0
    if path.name == "a-harfbuzz.ninja":
        s = re.sub(r"^(cxxflags_a-harfbuzz_[^\n]+)$", r'\1 /DHB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR', s, flags=re.M)
    if path.name == "SumatraPDF-static.ninja":
        gfx_source = "../../src/gui/Gfx.cpp"
        assert gfx_source in s
        s = s.replace(gfx_source, str(root / "local/Gfx.cpp"))
        for name in ("TabsCtrl.cpp", "Tooltip.cpp"):
            source = "../../src/gui/win/" + name
            assert source in s
            s = s.replace(source, str(root / "local" / name))
        s = s.replace("../../src/shared/TabsCommon.cpp", str(root / "local/TabsCommon.cpp"))
        s = s.replace("../../src/SumatraPDF.cpp", str(root / "local/SumatraPDF.cpp"))
        s = s.replace("../../src/shared/DisplayModel.cpp", str(root / "local/DisplayModel.cpp"))
        s = s.replace("../../src/shared/MenuDefs.cpp", str(root / "local/MenuDefs.cpp"))
        s = s.replace("../../src/shared/Commands.cpp", str(root / "local/Commands.cpp"))
        s = s.replace("../../src/shared/ReadAloud_win.cpp", str(root / "local/ReadAloud_win.cpp"))
        libdirs = [tools / "sdk/crt/lib/x86_64", tools / "sdk/sdk/lib/ucrt/x86_64", tools / "sdk/sdk/lib/um/x86_64"]
        def normalize_libs(match):
            tokens = match.group(0).split()
            for i, name in enumerate(tokens):
                if not name.lower().endswith(".lib") or "/" in name:
                    continue
                candidates = [p.name for d in libdirs for p in d.iterdir() if p.name.lower() == name.lower()]
                if not candidates:
                    raise RuntimeError("SDK library not found: " + name)
                tokens[i] = name if name in candidates else candidates[0]
            return " ".join(tokens)
        s = re.sub(r"(?m)^links_SumatraPDF-static_Release_x64 = [^\n]*", normalize_libs, s)
        stamp = "../../out/rel64/obj-s/SumatraPDF-static/SumatraPDF-static.prebuild"
        s = re.sub(r"(?m)^(build " + re.escape(stamp) + r": prebuild[^\n]*\n)  prebuildcommands = [^\n]*", r"\1  prebuildcommands = touch " + stamp, s)
        resource = "../../out/rel64/obj-s/SumatraPDF-static/SumatraPDF-static.res"
        marker = "build ../../out/rel64/SumatraPDF-static.exe: link_msc-v145 "
        assert marker in s
        archive = "../../out/rel64/embedded-static.lzsa"
        inputs = ["../../src/resource.h", "../../src/shared/Version.h", "../../src/shared/SumatraPDF.exe.manifest", "../../src/reloading.bmp"]
        inputs += ["../../src/gfx/" + p.name for p in sorted((root / "tree/src/gfx").glob("*.ico"))]
        edge = ("rule adapt_static_rc\n  command = python3 " + str(root / "adapt-resource.py") + " $in $out\n"
                "build SumatraPDF-linux.rc: adapt_static_rc ../../src/SumatraPDF.rc\n")
        edge += ("rule pack_static_linux\n  command = python3 " + str(root / "pack-static.py") + " $out\n  restat = 1\n"
                "build always_pack_static_linux: phony\n"
                "build " + archive + ": pack_static_linux | always_pack_static_linux\n"
                "build " + resource + ": rc_msc-v145 SumatraPDF-linux.rc | " + archive + " " + " ".join(inputs) + "\n"
                "  resflags = /I../../src /I../../src/shared /D EMBEDDED_PAK=../../out/rel64/embedded-static.lzsa\n")
        if os.environ.get("SUMATRA_REUSE_EMBEDDED") == "1":
            if not (root / "tree/out/rel64/embedded-static.lzsa").is_file():
                raise RuntimeError("existing embedded resource archive not found")
            edge = edge.replace(": pack_static_linux | always_pack_static_linux", ": pack_static_linux")
        s = s.replace(marker, edge + marker + resource + " ", 1)
    path.write_text(s)
    for line in s.splitlines():
        if line.startswith("build "):
            for output in line[6:].split(":", 1)[0].split():
                if output.startswith("../../out/"):
                    (dst / output).parent.mkdir(parents=True, exist_ok=True)
print("Adapted graph:", dst)
