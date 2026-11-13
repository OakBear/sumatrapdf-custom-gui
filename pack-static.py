#!/usr/bin/env python3
"""Stage SumatraPDF static resources and pack them with Wine MakeLZSA."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT.parent / "sumatrapdf"
if len(sys.argv) != 2:
    raise SystemExit("usage: pack-static.py OUTPUT.lzsa")
out = Path(sys.argv[1]).resolve()
out.parent.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env["WINEPREFIX"] = str(ROOT / "wine")
env.setdefault("WINEDEBUG", "-all")
def winpath(p):
    return "Z:" + str(p.resolve()).replace("/", "\\")
with tempfile.TemporaryDirectory(prefix="pack-", dir=ROOT) as tmp:
    stage = Path(tmp)
    work = ROOT / "tree/.work"
    docs = work / "docs"
    if docs.is_dir():
        shutil.copytree(docs, stage, dirs_exist_ok=True)
    translations = work / "translations.txt"
    if translations.is_file():
        shutil.copy2(translations, stage / "translations.txt")
    else:
        (stage / "translations.txt").touch()
    scripts = work / "scripts.js"
    if scripts.is_file():
        shutil.copy2(scripts, stage / "scripts.js")
    for name in ("marked.min.js", "pele.min.js", "microlighter.min.js"):
        shutil.copy2(SOURCE / "ext" / name, stage / name)
    if env.get("SUMATRA_NO_EMBED_FONTS") != "1":
        patterns = {"urw": ["Dingbats.cff", "NimbusMonoPS-*.cff", "NimbusRoman-*.cff", "NimbusSans-*.cff", "StandardSymbolsPS.cff"], "droid": ["DroidSansFallbackFull.ttf"], "sil": ["CharisSIL*.cff"], "noto": ["NotoSans-Regular.otf", "NotoSerif-Regular.otf", "NotoSansMath-Regular.otf", "NotoMusic-Regular.otf", "NotoSansSymbols-Regular.otf", "NotoSansSymbols2-Regular.otf", "NotoEmoji-Regular.ttf"]}
        dest = stage / "fonts"
        dest.mkdir()
        for family, names in patterns.items():
            for pattern in names:
                matches = list((SOURCE / "ext/mupdf/resources/fonts" / family).glob(pattern))
                if not matches:
                    raise SystemExit(f"missing font: {family}/{pattern}")
                for font in matches:
                    shutil.copy2(font, dest / font.name)
    subprocess.run(["wine", str(SOURCE / "bin/MakeLZSA.exe"), winpath(out), winpath(stage)], env=env, check=True)
