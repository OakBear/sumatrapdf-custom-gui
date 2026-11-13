#!/bin/bash
# 在当前 Linux 桌面上直接跑构建出来的 Windows EXE（Wine）。
# 用法：
#   ./run-on-desktop.sh                      # 打开起始页
#   ./run-on-desktop.sh ~/doc/a.pdf          # 打开某个 PDF
#   DISPLAY=:0 ./run-on-desktop.sh           # 指定桌面（VNC 默认就是 :0）
set -eu
here=$(cd "$(dirname "$0")" && pwd)
export DISPLAY="${DISPLAY:-:0}"
# Wine 前缀放本地磁盘：/tmp 比 BeeGFS 上的 $HOME 快一个数量级（几千个小文件）
if [ -z "${WINEPREFIX:-}" ]; then
    if [ -d /tmp/spw-pfx/drive_c ]; then
        WINEPREFIX=/tmp/spw-pfx
    else
        WINEPREFIX=/tmp/wine-sumatra
    fi
fi
export WINEPREFIX
export WINEDEBUG="${WINEDEBUG:--all}"

# 配置（含书签）放家目录，重启桌面也还在
APPDATA_UNIX="${SUMATRA_APPDATA:-$HOME/sumatra-bm-appdata}"
mkdir -p "$APPDATA_UNIX"

if [ ! -d "$WINEPREFIX/drive_c" ]; then
    echo "首次运行：初始化 Wine 前缀 $WINEPREFIX（本地磁盘，几十秒，只做一次）"
    wineboot -i >/dev/null 2>&1 || true
fi

EXE="$here/dist/SumatraPDF-static-bookmarks.exe"
[ -f "$EXE" ] || { echo "找不到 $EXE，先跑 ./build-static.sh"; exit 1; }

WINAPPDATA=$(winepath -w "$APPDATA_UNIX" 2>/dev/null | tr -d '\r')
echo "DISPLAY=$DISPLAY  WINEPREFIX=$WINEPREFIX  配置目录=$APPDATA_UNIX"

args=()
for p in "$@"; do
    case "$p" in
        -*) args+=("$p") ;;
        *)  args+=("$(winepath -w "$p" 2>/dev/null | tr -d '\r')") ;;
    esac
done
exec wine "$EXE" -appdata "$WINAPPDATA" "${args[@]}"
