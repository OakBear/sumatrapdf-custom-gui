#!/usr/bin/env python3
"""Bundle the bookmark PDF scripts into the resource-pack input.

SumatraPDF's "Run PDF Script..." dialog reads embedded scripts.js, split on
"// ---" with an optional "// Info: <name>" header per script. Upstream
generates that file from the private sumatra-website checkout, which this
build does not have, so the example list is otherwise empty.

Run this, then `bash ../build-static.sh` from sumatrapdf-build/, to bake the
bookmark scripts into SumatraPDF-static.exe itself.

By default the "// Arg:" lines are STRIPPED from the bundle: under Wine 6.0.2
the dialog crashes as soon as it creates an argument row (reproducible on the
stock unmodified EXE), so the bundled scripts use their CONFIG block instead.
Use --keep-arg-rows only if you run on real Windows and want prefilled fields.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "tree" / ".work" / "scripts.js"
ORDER = [
    "add-one-bookmark.js",
    "add-pdf-bookmarks.js",
    "dump-pdf-bookmarks.js",
]
SEPARATOR = "\n// ---\n"

keep_arg_rows = "--keep-arg-rows" in sys.argv[1:]

parts = []
for name in ORDER:
    text = (HERE / name).read_text(encoding="utf-8").replace("\r\n", "\n")
    if not keep_arg_rows:
        text = "\n".join(l for l in text.split("\n") if not l.strip().startswith("// Arg:"))
    source = text.rstrip()
    if SEPARATOR.strip() in source:
        raise SystemExit(f"{name}: contains the script separator")
    if not source.strip():
        raise SystemExit(f"{name}: empty script")
    if not source.startswith("// Info:"):
        raise SystemExit(f"{name}: missing '// Info:' header (needed for the examples list)")
    parts.append(source)

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(SEPARATOR.join(parts) + "\n", encoding="utf-8")
mode = "with argument rows (real Windows only)" if keep_arg_rows else "without argument rows (Wine-safe)"
print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {len(parts)} scripts, {mode})")
for name in ORDER:
    header = (HERE / name).read_text(encoding="utf-8").splitlines()[0]
    print(f"  - {header[len('// Info:'):].strip()}")
print("now run: bash build-static.sh   (from sumatrapdf-build/)")
