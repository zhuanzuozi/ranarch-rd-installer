#!/usr/bin/env bash
# RanArch Installer —— 启动器（桌面快捷方式指向它）
#
# 三条路径，按顺序尝试：
#
#   ① 系统守护进程可用（/run/ranarch/ranarch.sock 存在且当前用户能 stat 到，
#      也就是已经在 ranarch 组里）→ 直接用。它是常驻服务，不随前端退出。
#
#   ② 否则用 pkexec 拉起「会话级后端」：以 root 运行（安装需要），但 socket 建在
#      $XDG_RUNTIME_DIR 下并 chown 给当前用户（0600），只有本人能连；同时带
#      --exit-with-pid 盯住前端进程，前端一退出它就自己结束。首次会弹一次系统
#      认证框（polkit 动作 org.ranarch.daemon.launch）。
#
#   ③ pkexec 不可用或认证被取消 → 退回测试模式：当前用户前台跑仓库里构建出来的
#      守护进程，路径全落在 /tmp/rana-test，只能做解析/校验类操作。
#      同样带 --exit-with-pid，保证前端退出后不留后台进程。
#
# 关键点：本脚本最后用 exec 启动 GUI，所以 $$ 在 exec 前后是同一个进程号 ——
# 交给守护进程盯的就是 GUI 自己。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAEMON_SYS=/usr/bin/ranarch-daemon
DAEMON_DEV="$ROOT/build/ranarch-daemon"
SYS_SOCK=/run/ranarch/ranarch.sock
SYS_CONF=/etc/ranarch/ranarch.conf
TEST_DIR="${RANARCH_TEST_DIR:-/tmp/rana-test}"
GUI="$(ls -1 "$ROOT"/ui/release/RanArch-Installer-*.AppImage 2>/dev/null | tail -1 || true)"

if [ -z "$GUI" ]; then
  echo "[launcher] 找不到 AppImage，请先打包：cd '$ROOT/ui' && npm run dist" >&2
  exit 1
fi

RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/ranarch-session-$(id -u)}"
SESS_SOCK="$RUNTIME_DIR/ranarch.sock"
SESS_CONF="$RUNTIME_DIR/ranarch-session.conf"
SESS_LOG="$RUNTIME_DIR/ranarch-session.log"

# socket 是否「可用」：存在还不够，必须我们真的连得上（有写权限）。
# 否则会出现 socket 在、但属主是 root、连不上的假就绪状态。
socket_usable() {
  [ -S "$1" ] && [ -w "$1" ]
}

# 等 socket 就绪（pkexec 要等用户输密码，所以给宽一点）
wait_for_socket() {
  local sock="$1" pid="$2" tries="$3"
  for _ in $(seq 1 "$tries"); do
    socket_usable "$sock" && return 0
    [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null && return 1
    sleep 0.3
  done
  return 1
}

# ---- ① 系统守护进程 ----
if socket_usable "$SYS_SOCK"; then
  echo "[launcher] 使用系统守护进程（$SYS_SOCK，常驻服务，不随前端退出）"
  exec env RANARCH_SOCKET="$SYS_SOCK" RANARCH_CONFIG="$SYS_CONF" \
       "$GUI" --no-sandbox "$@"
fi

# ---- ② 会话级后端 ----
if [ -x "$DAEMON_SYS" ] && command -v pkexec >/dev/null 2>&1; then
  mkdir -p "$RUNTIME_DIR"
  chmod 700 "$RUNTIME_DIR" 2>/dev/null || true

  # 会话配置：以系统配置为底（真实路径，装得上东西），只把 socket 和日志挪到
  # 本会话目录，免得和系统守护进程抢文件。
  BASE_CONF="$SYS_CONF"
  [ -r "$BASE_CONF" ] || BASE_CONF="$ROOT/data/ranarch.conf.example"
  sed -e "s#^\(socket_path[[:space:]]*=\).*#\1 $SESS_SOCK#" \
      -e "s#^\(log_file[[:space:]]*=\).*#\1 $SESS_LOG#" \
      "$BASE_CONF" > "$SESS_CONF"

  # 本会话已经有能连的后端 → 直接复用
  if socket_usable "$SESS_SOCK" && pgrep -f -- "--config $SESS_CONF" >/dev/null 2>&1; then
    echo "[launcher] 复用本会话已在运行的守护进程"
    exec env RANARCH_SOCKET="$SESS_SOCK" RANARCH_CONFIG="$SESS_CONF" \
         "$GUI" --no-sandbox "$@"
  fi

  rm -f "$SESS_SOCK"      # 清掉上次崩溃留下的、连不上的僵尸 socket
  echo "[launcher] 用 pkexec 拉起会话级守护进程（会弹一次系统认证框）"
  pkexec "$DAEMON_SYS" --config "$SESS_CONF" -f \
         --exit-with-pid=$$ --socket-owner="$(id -u)" &
  PKEXEC_PID=$!
  if wait_for_socket "$SESS_SOCK" "$PKEXEC_PID" 100; then
    echo "[launcher] 会话级守护进程就绪（socket 归 $(id -un) 所有，0600）"
    exec env RANARCH_SOCKET="$SESS_SOCK" RANARCH_CONFIG="$SESS_CONF" \
         "$GUI" --no-sandbox "$@"
  fi
  # 不杀 pkexec：认证被取消时它自己就退了；若是它起得慢，那个守护进程也盯着
  # 前端 pid，前端一退它照样会自己结束，不会变成野进程。
  echo "[launcher] pkexec 没在 30s 内就绪（认证取消？），退回测试模式" >&2
fi

# ---- ③ 测试模式：本用户前台跑构建产物，只写临时目录 ----
SOCK="$TEST_DIR/ranarch.sock"
CONF="$TEST_DIR/ranarch.conf"

if [ ! -x "$DAEMON_DEV" ]; then
  echo "[launcher] 找不到 $DAEMON_DEV，请先构建后端：" >&2
  echo "           cmake -S '$ROOT' -B '$ROOT/build' -DCMAKE_BUILD_TYPE=Release && cmake --build '$ROOT/build' -j\"\$(nproc)\"" >&2
  exit 1
fi

mkdir -p "$TEST_DIR"
if [ ! -f "$CONF" ]; then
  sed \
    -e "s#/var/lib/ranarch/staging#$TEST_DIR/staging#" \
    -e "s#/var/lib/ranarch/ranarch.db#$TEST_DIR/ranarch.db#" \
    -e "s#/etc/ranarch/dep_map.csv#$ROOT/data/dep_map.csv#" \
    -e "s#/usr/share/ranarch/keyring#$TEST_DIR/keyring#" \
    -e "s#/var/lib/ranarch/keyring#$TEST_DIR/runtime-keyring#" \
    -e "s#/var/log/ranarch/ranarch.log#$TEST_DIR/ranarch.log#" \
    -e "s#/run/ranarch/ranarch.sock#$SOCK#" \
    "$ROOT/data/ranarch.conf.example" > "$CONF"
  echo "[launcher] 已生成测试配置 $CONF"
fi

if ! { socket_usable "$SOCK" && pgrep -f -- "--config $CONF" >/dev/null 2>&1; }; then
  rm -f "$SOCK"
  echo "[launcher] 启动测试守护进程（socket: $SOCK）"
  nohup "$DAEMON_DEV" --config "$CONF" -f --exit-with-pid=$$ \
        > "$TEST_DIR/daemon.log" 2>&1 &
  if ! wait_for_socket "$SOCK" "" 30; then
    echo "[launcher] 守护进程没起来，看日志：$TEST_DIR/daemon.log" >&2
    tail -5 "$TEST_DIR/daemon.log" >&2 || true
    exit 1
  fi
  echo "[launcher] 测试守护进程就绪"
fi

echo "[launcher] 启动 GUI: $(basename "$GUI")"
exec env RANARCH_SOCKET="$SOCK" RANARCH_CONFIG="$CONF" "$GUI" --no-sandbox "$@"
