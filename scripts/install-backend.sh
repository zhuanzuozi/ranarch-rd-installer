#!/usr/bin/env bash
# 把「和本脚本放在一起」的后端产物装进系统 —— 需要 root，通常由 pkexec 调用。
#
# 约定：脚本认为自己旁边就是这个布局（由 scripts/stage-backend.sh 或
# electron-builder 的 extraResources 产出）：
#
#   install-backend.sh
#   bin/ranarch-daemon  bin/ranarchctl
#   lib/libranarch.so.0
#   polkit/org.ranarch.daemon.policy
#   systemd/ranarch-daemon.service
#   etc/ranarch.conf.example  etc/dep_map.csv
#
# 这样前端（AppImage 里的主进程）只要把整个目录摆好，就可以直接 pkexec 本脚本，
# 不需要用户自己去 cmake --install。
#
# 幂等：重复执行只是覆盖同名文件；/etc/ranarch/ranarch.conf 已存在时绝不覆盖。
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX=/usr

say()  { printf '%s\n' "$*"; }
die()  { printf '错误：%s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "需要 root 权限（正常由 pkexec / sudo 调用）"

# ---- 0) 校验载荷齐不齐 ----
for f in bin/ranarch-daemon bin/ranarchctl lib/libranarch.so.0 \
         polkit/org.ranarch.daemon.policy systemd/ranarch-daemon.service \
         etc/ranarch.conf.example etc/dep_map.csv; do
    [ -e "$SRC/$f" ] || die "载荷不完整，缺少 $SRC/$f"
done
say "载荷目录：$SRC"

# ---- 1) ranarch 组 ----
# systemd unit 里写了 Group=ranarch，组不存在服务会起不来。
if getent group ranarch >/dev/null 2>&1; then
    say "系统组 ranarch 已存在"
else
    groupadd --system ranarch
    say "已创建系统组 ranarch"
fi

# ---- 2) 可执行文件与共享库 ----
install -Dm755 "$SRC/bin/ranarch-daemon" "$PREFIX/bin/ranarch-daemon"
install -Dm755 "$SRC/bin/ranarchctl"     "$PREFIX/bin/ranarchctl"
install -Dm755 "$SRC/lib/libranarch.so.0" "$PREFIX/lib/libranarch.so.0"
# 顺便给一个不带版本号的软链，方便本地编译链接
ln -sfn libranarch.so.0 "$PREFIX/lib/libranarch.so"
say "已安装 ranarch-daemon / ranarchctl / libranarch.so.0"

# ---- 3) polkit 策略与 systemd unit ----
install -Dm644 "$SRC/polkit/org.ranarch.daemon.policy" \
        "$PREFIX/share/polkit-1/actions/org.ranarch.daemon.policy"
install -Dm644 "$SRC/systemd/ranarch-daemon.service" \
        "$PREFIX/lib/systemd/system/ranarch-daemon.service"
say "已安装 polkit 策略与 systemd unit"

# ---- 4) 配置 ----
install -Dm644 "$SRC/etc/ranarch.conf.example" /etc/ranarch/ranarch.conf.example
# dep_map.csv 是随版本更新的数据表：覆盖前先备份用户改过的版本
if [ -e /etc/ranarch/dep_map.csv ] && ! cmp -s "$SRC/etc/dep_map.csv" /etc/ranarch/dep_map.csv; then
    cp -a /etc/ranarch/dep_map.csv /etc/ranarch/dep_map.csv.bak
    say "已把旧的 dep_map.csv 备份为 /etc/ranarch/dep_map.csv.bak"
fi
install -Dm644 "$SRC/etc/dep_map.csv" /etc/ranarch/dep_map.csv
# 真正生效的配置：只在缺失时创建，绝不覆盖用户改过的
if [ -e /etc/ranarch/ranarch.conf ]; then
    say "已存在 /etc/ranarch/ranarch.conf，保持不动"
else
    install -Dm644 "$SRC/etc/ranarch.conf.example" /etc/ranarch/ranarch.conf
    say "已生成 /etc/ranarch/ranarch.conf（来自示例）"
fi

# ---- 5) 运行期目录 ----
for d in /var/lib/ranarch /var/lib/ranarch/staging /var/lib/ranarch/keyring \
         /var/log/ranarch /run/ranarch /usr/share/ranarch/keyring; do
    install -d -m 0755 "$d"
done
say "已确保运行期目录存在"

# ---- 6) systemd ----
# 故意不 enable：前端走的是「会话级后端」（前端退出后端就退出），
# 常驻服务是另一条路，交给用户自己决定。
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
    if systemctl is-active --quiet ranarch-daemon 2>/dev/null; then
        systemctl restart ranarch-daemon || true
        say "检测到常驻服务在跑，已重启以加载新二进制"
    fi
fi

say ""
say "后端安装完成。"
say "  - 前端会自动用它拉起会话级后端（前端退出即结束）"
say "  - 想改成开机常驻：systemctl enable --now ranarch-daemon"
say "    （常驻模式下 socket 是 /run/ranarch/ranarch.sock，属 root:ranarch 0750，"
say "      当前用户需要在 ranarch 组里，且加组后要重新登录）"
