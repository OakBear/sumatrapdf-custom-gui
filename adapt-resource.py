#!/usr/bin/env python3
"""Convert Windows resource filename separators for llvm-rc on Linux."""
from pathlib import Path
import re
import sys
source, output = map(Path, sys.argv[1:])
text = source.read_text()
text = re.sub(r'"gfx\\\\([^"\n]+)"', r'"gfx/\1"', text)
output.write_text(text)
