#!/usr/bin/env bash
# RanArch Installer —— 桌面快捷方式指向它。
#
# 后端（root 守护进程）由 AppImage 里的主进程自己负责，见 ui/electron/main.ts：
#   - 系统守护进程可用（/run/ranarch/ranarch.sock 能连）→ 直接用；
#   - 否则按需一次性把随包后端装进 /usr（会弹一次 pkexec 认证框），
#     再拉起「前端退出它就退出」的会话级后端。
# 所以这个脚本只负责把 AppImage 起起来。
#
# 调试时可以绕开这一切，直接把前端指到任意守护进程：
#   RANARCH_SOCKET=/tmp/x.sock RANARCH_CONFIG=/tmp/x.conf bash scripts/dev-launch.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUI="$(ls -1 "$ROOT"/ui/release/RanArch-Installer-*.AppImage 2>/dev/null | tail -1 || true)"

if [ -z "$GUI" ]; then
    echo "找不到 AppImage，请先打包：cd '$ROOT/ui' && npm run dist:appimage" >&2
    exit 1
fi

exec "$GUI" --no-sandbox "$@"
