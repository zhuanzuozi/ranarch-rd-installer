# RanArch Installer

在 Arch Linux 及其衍生系统（Manjaro、EndeavourOS…）上**直接安装 `.deb` / `.rpm` 包**的图形安装器。

Arch 用的是 pacman 和 `.pkg.tar.zst`，外来的 deb / rpm 本来装不上。RanArch 把这两种格式解析成统一的包描述，把依赖映射到 Arch 包名，检查冲突和签名，先在沙箱里试装一遍，确认没问题再由 root 守护进程落盘。

---

## 功能

| 环节 | 说明 |
| --- | --- |
| 解析 | `.deb`（`ar` + `control.tar.*` / `data.tar.*`）与 `.rpm`（lead + header），取出包名、版本、架构、依赖、文件清单。大包走流式读取，内存占用与包体积无关。 |
| 依赖映射 | Debian / RPM 依赖名 → Arch 包名，表在 `data/dep_map.csv`，可直接编辑。映射不到的按配置项 `default_action` 处理（`ask` / `skip` / `abort`）。 |
| 冲突检查 | 三类：与 pacman 已装包冲突、与 RanArch 已管包冲突、文件系统上已存在同名文件。 |
| 签名校验 | deb 的 `_gpgorigin`、rpm 的 Signature Header，经 gpgme 走信任钥匙圈。策略 `strict` / `warn` / `ignore`。 |
| 沙箱试装 | bubblewrap，只读根 + overlayfs 写层，试装过程不碰真实系统。`bwrap` 不可用时自动降级并明确告知。 |
| 实时终端 | `forkpty` + `epoll`，安装过程（含 pacman 自己的输出）逐字节流到前端，可选中复制。 |
| 特权授权 | 安装 / 卸载 / 管理密钥之前，守护进程先经 polkit 向发起方申请授权；拿不到授权就直接拒绝，不做任何系统改动。 |
| 会话级后端 | 前端退出，后端跟着退出（`pidfd` 监听），不留常驻 root 进程。 |
| 记录 | SQLite 保存已安装包、签名状态、信任密钥。 |

安装流水线（前端逐步显示）：解析 → 验签 → 依赖解析 → 冲突检查 → 沙箱试装 → 落盘暂存 → 安装 → 记录。

## 架构

```
ui/                     Electron 前端（Vite + TypeScript，无框架）
  ├ electron/main.ts      主进程：多窗口编排、/proc 采样、socket 桥
  └ src/                  渲染层：主界面恒定 600×600，队列与控制台是
                          钉在共享边上的独立透明窗口，靠 CSS transform 滑入
src/core/               libranarch.so —— 解析、依赖、冲突、签名、沙箱、PTY、DB
src/daemon/             ranarch-daemon —— root 守护进程，Unix socket + NDJSON
src/cli/                ranarchctl —— 命令行客户端
data/                   配置样例、依赖映射表
polkit/                 polkit 策略
systemd/                守护进程 unit
scripts/                桌面快捷方式启动器
tests/                  ctest 用例
```

前端与守护进程之间是 Unix socket 上的 NDJSON（4 字节长度前缀 + JSON）。请求类型 `list` / `info` / `install` / `remove`；事件类型 `session_start` / `step` / `progress` / `log` / `pty_data` / `error` / `done`。

## 依赖

```bash
sudo pacman -S --needed base-devel cmake pkgconf libarchive sqlite gpgme pacman polkit
```

`spdlog` 与 `nlohmann-json` 是**可选**的：日志自实现，JSON 用仓库内的 `src/core/json.h`，所以最小 Arch 环境也能直接编。polkit 缺失时能编过，但特权操作会一律拒绝（fail-closed）。

前端另需 `nodejs` 与 `npm`。

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure     # 7 个用例
```

打包前端（产出 `ui/release/RanArch-Installer-<版本>-x86_64.AppImage`）：

```bash
cd ui && npm install && npx electron-builder --linux AppImage
```

## 安装系统组件（可选）

想让后端常驻、或走系统级 socket，就把守护进程装进系统：

```bash
sudo cmake --install build          # 装到 /usr/{bin,lib,share}、/etc/ranarch、polkit 策略、systemd unit
sudo systemctl enable --now ranarch-daemon
```

不装也能用：桌面快捷方式（`scripts/dev-launch.sh`）会用 `pkexec` 现拉一个**会话级后端**，socket 建在 `$XDG_RUNTIME_DIR` 下并归当前用户所有（`0600`），前端退出它就自己结束。

## 用法

图形界面：双击桌面快捷方式，或直接运行 AppImage。把 `.deb` / `.rpm` 拖进中间的六边形，点安装；首次会弹 polkit 认证框，之后一段时间内不再重复询问。

命令行：

```bash
ranarchctl install foo.deb              # 装
ranarchctl install foo.rpm --dry-run    # 只做沙箱试装，不真正写盘
ranarchctl install foo.deb --sandbox    # 正式安装前先试装
ranarchctl list                         # 列已管包
ranarchctl remove <id>
ranarchctl info <session_id>

# 其余开关：--no-signature --no-dependencies --no-conflicts
#           --no-path-traversal（危险）  --force（等价于上面全部 --no-*）
# 全局：    --socket <path>（默认 /run/ranarch/ranarch.sock）
```

## 配置

`/etc/ranarch/ranarch.conf`，INI 格式；键与默认值见 [`data/ranarch.conf.example`](data/ranarch.conf.example)。改完发 `SIGHUP` 热重载。测试或会话级运行时可用环境变量 `RANARCH_CONFIG` / `RANARCH_SOCKET` 覆盖路径。

## 已知限制

- **不执行包自带的维护脚本**（`postinst` / `prerm` / `%post` 等）。deb 的 control 成员会被解析，但脚本内容不会运行，所以依赖脚本副作用的包装完可能还需要手动收尾。
- 前端 `npm run dist` 里的 deb 打包目标需要 `libxcrypt-compat`（electron-builder 自带的 fpm 依赖 `libcrypt.so.1`）。只出 AppImage 用上面那条 `electron-builder --linux AppImage`。

## 许可

GPL-3.0-or-later —— 各源码文件头部的 SPDX 标识为准。
