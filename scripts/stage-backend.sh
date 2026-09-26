#!/usr/bin/env bash
# 把后端产物整理成 install-backend.sh 认识的布局，输出到 ui/backend/，
# 再由 electron-builder 的 extraResources 打进 AppImage —— 前端和后端就此合成一个文件。
#
# 做法是借道 CMake 自己的安装规则（DESTDIR 装到临时目录再重新摆放），
# 这样安装布局只有 CMakeLists.txt 一份定义，不会两处不同步。
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
OUT="$ROOT/ui/backend"
CMAKE="${CMAKE:-$(command -v cmake || echo /usr/bin/cmake)}"
READELF="${READELF:-$(command -v readelf || echo /usr/bin/readelf)}"

[ -x "$BUILD/ranarch-daemon" ] || {
    echo "错误：找不到 $BUILD/ranarch-daemon，请先构建后端：" >&2
    echo "      cmake -S '$ROOT' -B '$BUILD' -DCMAKE_BUILD_TYPE=Release && cmake --build '$BUILD' -j\"\$(nproc)\"" >&2
    exit 1
}

# 安装前缀从构建缓存里读（Arch 的 cmake 默认 /usr）
PREFIX="$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$BUILD/CMakeCache.txt" | head -1)"
[ -n "$PREFIX" ] || PREFIX=/usr

rm -rf "$OUT"
mkdir -p "$OUT" "$OUT/.root"
# cmake --install 会往构建目录写 install_manifest.txt。如果之前用 sudo/pkexec
# 装过，这个文件是 root 属主，普通用户写不进去 —— 直接删掉（它只是记录用）。
if [ -e "$BUILD/install_manifest.txt" ] && [ ! -w "$BUILD/install_manifest.txt" ]; then
    rm -f "$BUILD/install_manifest.txt"
fi
DESTDIR="$OUT/.root" "$CMAKE" --install "$BUILD" --prefix "$PREFIX" >/dev/null

# 把 DESTDIR 树重新摆成 install-backend.sh 认识的扁平布局
mv "$OUT/.root$PREFIX/bin" "$OUT/bin"
mkdir -p "$OUT/lib"
# -L 解引用软链：只保留加载器真正要找的那个 SONAME，且是真文件，
# 免得打包时软链被展开成三份副本
cp -L "$OUT/.root$PREFIX/lib/libranarch.so.0" "$OUT/lib/libranarch.so.0"
mkdir -p "$OUT/polkit" "$OUT/systemd" "$OUT/etc"
mv "$OUT/.root$PREFIX/share/polkit-1/actions/org.ranarch.daemon.policy" "$OUT/polkit/"
mv "$OUT/.root$PREFIX/lib/systemd/system/ranarch-daemon.service" "$OUT/systemd/"
mv "$OUT/.root/etc/ranarch/ranarch.conf.example" "$OUT/etc/"
mv "$OUT/.root/etc/ranarch/dep_map.csv" "$OUT/etc/"
rm -rf "$OUT/.root"

install -m 755 "$ROOT/scripts/install-backend.sh" "$OUT/install-backend.sh"

echo "已生成 $OUT"
echo "--- 布局 ---"
( cd "$OUT" && find . -type f -printf '  %10s  %p\n' | sort -k2 )
echo "--- rpath 自检（应为 \$ORIGIN/../lib）---"
for b in bin/ranarch-daemon bin/ranarchctl; do
    runpath=$("$READELF" -d "$OUT/$b" 2>/dev/null | sed -n 's/.*RUNPATH.*\[\(.*\)\]/\1/p')
    printf '  %-22s RUNPATH=%s\n' "$b" "${runpath:-<空>}"
done
echo "--- 脱离 /usr 的加载自检 ---"
( cd "$OUT" && ldd bin/ranarch-daemon | grep libranarch | sed 's/^/  /' )
echo "--- 总体积 ---"
du -sh "$OUT" | sed 's/^/  /'
