#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
toolchain="$root/../sumatrapdf-toolchain"
sdk="$toolchain/sdk"
export INCLUDE="$sdk/crt/include;$sdk/sdk/include/ucrt;$sdk/sdk/include/shared;$sdk/sdk/include/um;$sdk/sdk/include/winrt"
mkdir -p "$root/local/sdk-lib-aliases"
for name in Rstrtmgr Bcrypt PowrProf; do
    lower="${name,,}"
    ln -sfn "$sdk/sdk/lib/um/x86_64/$lower.lib" "$root/local/sdk-lib-aliases/$name.lib"
done
export LIB="$root/local/sdk-lib-aliases;$sdk/crt/lib/x86_64;$sdk/sdk/lib/ucrt/x86_64;$sdk/sdk/lib/um/x86_64"
if (( $# )); then
    echo "This script builds only SumatraPDF-static Release x64; no arguments are accepted." >&2
    exit 2
fi
"$toolchain/bin/ninja" -C "$root/tree/.work/linux" -j4 SumatraPDF-static_Release_x64 2>&1 | tee -a "$root/build-static.log"
