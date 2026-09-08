#!/bin/sh
# 无 root / 纯终端环境构建 jellyfin4vlc（Debian/Ubuntu）。
#
# 原理：apt-get download 不需要 root；把 dev 包用 dpkg -x 解压到项目
# .deps/prefix 里，编译时用 PKG_CONFIG_SYSROOT_DIR 重定向，运行时用
# LD_LIBRARY_PATH + VLC_PLUGIN_PATH。全程不碰系统目录。
#
# 用法： sh scripts/setup-noroot.sh build   # 解压依赖 + 编译
#        sh scripts/setup-noroot.sh test    # 用解压出的 VLC 无界面运行插件
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
DEPS=$ROOT/.deps
PREFIX=$DEPS/prefix

# Debian 13+：插件头文件在 libvlccore-dev（没有 vlc-plugin-dev）；
# Ubuntu 老版本是 vlc-plugin-dev，按需调整下面这行。
DEV_PKGS="libvlccore-dev libvlc-dev libcurl4-openssl-dev"
RUN_PKGS="vlc-bin vlc-plugin-base vlc-data libvlc5 libvlccore9"

case "${1:-build}" in
build)
    mkdir -p "$DEBS_DIR" "$PREFIX/usr" 2>/dev/null || true
    mkdir -p "$DEPS/debs" "$PREFIX"
    cd "$DEPS/debs"
    # 开发包
    apt-get download $DEV_PKGS
    # 运行时依赖闭包 + libcurl 运行时（t64 命名按发行版调整）
    apt-cache depends --recurse --no-recommends --no-suggests \
        --no-conflicts --no-breaks --no-replaces --no-enhances \
        $RUN_PKGS 2>/dev/null | grep "^\w" | sort -u > pkgs.txt
    xargs -n50 apt-get download -y < pkgs.txt || true
    apt-get download libcurl4t64 2>/dev/null || apt-get download libcurl4 || true
    for d in *.deb; do dpkg -x "$d" "$PREFIX"; done
    cd "$ROOT"
    # dev 包里的 libcurl.so 若是指向不存在目标的链接，重建之
    (
        cd "$PREFIX/usr/lib/$(gcc -dumpmachine)" 2>/dev/null ||
        cd "$PREFIX/usr/lib/x86_64-linux-gnu"
        [ -e libcurl.so.4 ] && ln -sf libcurl.so.4 libcurl.so || true
    )
    PKG_CONFIG_SYSROOT_DIR=$PREFIX \
    PKG_CONFIG_PATH=$PREFIX/usr/lib/x86_64-linux-gnu/pkgconfig \
    make CURL_CFLAGS="-I$PREFIX/usr/include/x86_64-linux-gnu" \
         LDFLAGS="-L$PREFIX/usr/lib/x86_64-linux-gnu"
    echo "OK: $ROOT/libjellyfin_plugin.so"
    ;;

test)
    PLUGDIR=$PREFIX/usr/lib/x86_64-linux-gnu/vlc/plugins
    mkdir -p "$PLUGDIR/interface"
    cp "$ROOT/libjellyfin_plugin.so" "$PLUGDIR/interface/"
    LD_LIBRARY_PATH=$PREFIX/usr/lib/x86_64-linux-gnu \
        "$PLUGDIR/../vlc/vlc-cache-gen" "$PLUGDIR"
    echo "--- vlc --list | grep jellyfin ---"
    LD_LIBRARY_PATH=$PREFIX/usr/lib/x86_64-linux-gnu \
    VLC_PLUGIN_PATH=$PLUGDIR \
        "$PREFIX/usr/bin/vlc" -I dummy --list 2>/dev/null | grep -i jellyfin
    echo "--- 启动插件（需要一个能访问的 Jellyfin 服务器）---"
    echo "例： $PREFIX/usr/bin/vlc -I dummy --extraintf jellyfin \\"
    echo "       --jellyfin-server http://SERVER:8096 \\"
    echo "       --jellyfin-username USER --jellyfin-password PASS somefile.mkv"
    ;;

*)
    echo "用法: $0 [build|test]" >&2; exit 1 ;;
esac
