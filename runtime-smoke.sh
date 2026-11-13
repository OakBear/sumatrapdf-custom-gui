#!/usr/bin/env bash
# Run after the rel64 build has completed; this does not build anything.
set -u
umask 077

base=/cluster/home/it_stu171/rea/sumatrapdf-build
exe="$base/tree/out/rel64/SumatraPDF-static.exe"
pdf=/cluster/home/it_stu171/rea/sumatrapdf/docs/test/zlib.3.pdf
prefix="$base/wine"

for cmd in wine xvfb-run timeout grep mktemp; do
    command -v "$cmd" >/dev/null 2>&1 || { printf "Missing command: %s\n" "$cmd" >&2; exit 2; }
done
for file in "$exe" "$pdf"; do
    [[ -f "$file" && -r "$file" ]] || { printf "Missing or unreadable file: %s\n" "$file" >&2; exit 2; }
done
[[ -d "$prefix" && -f "$prefix/system.reg" ]] || { printf "Missing initialized Wine prefix: %s\n" "$prefix" >&2; exit 2; }

logdir=$(mktemp -d "${TMPDIR:-/tmp}/sumatrapdf-runtime-smoke.XXXXXXXX") || exit 2
printf "Logs: %s\n" "$logdir"
export WINEPREFIX="$prefix"
export WINEDEBUG=-all
# Wine normally maps Z: to /; specify the Windows path explicitly.
winpdf='Z:\cluster\home\it_stu171\rea\sumatrapdf\docs\test\zlib.3.pdf'
# Upstream TestRenderPage passes the percent value directly as an engine scale.
# -zoom 1 therefore requests a 1x raster in this test mode.
run_case() {
    local page=$1 log=$2 status
    printf "Running page %s; log: %s\n" "$page" "$log"
    if timeout --kill-after=10s 90s xvfb-run -a wine "$exe" -console -render "$page" -zoom 1 "$winpdf" >"$log" 2>&1; then
        status=0
    else
        status=$?
    fi
    printf "exit status: %s\n" "$status"
    return "$status"
}

failed=0
run_case 1 "$logdir/page-1.log" || failed=1
if ! grep -Fq "rendering page 1 for " "$logdir/page-1.log"; then
    printf "FAIL: page 1 marker missing\n" >&2; failed=1
fi
if grep -Eiq "error|failed|invalid page number|no file provided|pageNumber is -1" "$logdir/page-1.log"; then
    printf "FAIL: page 1 log reports an error\n" >&2; failed=1
fi
run_case 999 "$logdir/page-999.log" || failed=1
if ! grep -Fq "invalid page number 999 (document has " "$logdir/page-999.log"; then
    printf "FAIL: negative control diagnostic missing\n" >&2; failed=1
fi
if (( failed )); then
    printf "FAIL: inspect logs in %s\n" "$logdir" >&2
    exit 1
fi
printf "PASS: page 1 and invalid-page control; logs: %s\n" "$logdir"
