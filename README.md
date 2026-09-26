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
  ├ electron/main.ts      主进程：多窗口编排、/proc 采样、socket 桥、后端生命周期
  └ src/                  渲染层：主界面恒定 600×600，队列与控制台是
                          钉在共享边上的独立透明窗口，靠 CSS transform 滑入
src/core/               libranarch.so —— 解析、依赖、冲突、签名、沙箱、PTY、DB
src/daemon/             ranarch-daemon —— root 守护进程，Unix socket + NDJSON
src/cli/                ranarchctl —— 命令行客户端
data/                   配置样例、依赖映射表
polkit/                 polkit 策略
systemd/                守护进程 unit
scripts/                桌面快捷方式启动器、打包与安装后端
tests/                  ctest 用例
```

前端与守护进程之间是 Unix socket 上的 NDJSON（4 字节长度前缀 + JSON）。请求类型 `list` / `info` / `install` / `remove`；事件类型 `session_start` / `step` / `progress` / `log` / `pty_data` / `error` / `done`。

### 前后端是「两个进程、一个文件」

后端必须以 root 跑（要往 `/` 写文件、要调 pacman），前端绝不能提权，所以两边是
两个进程 + 一个 Unix socket。但**分发形态**是合并的：后端产物（守护进程、CLI、
`libranarch.so`、polkit 策略、systemd unit、默认配置）由 `scripts/stage-backend.sh`
整理进 `ui/backend/`，再由 electron-builder 作为 `extraResources` 打进 AppImage。
用户只需要下载一个 AppImage。

启动时 [ui/electron/main.ts](ui/electron/main.ts) 按下面的顺序把后端准备好：

1. 系统守护进程可用（`/run/ranarch/ranarch.sock` 能连）→ 直接用，不插手；
2. 本会话已有会话级后端 → 复用；
3. 否则：`/usr/bin/ranarch-daemon` 不存在时，先用一次 pkexec 跑随包的
   `install-backend.sh` 把它装进 `/usr`；然后生成会话配置，用 pkexec 拉起
   `--exit-with-pid=<前端 pid> --socket-owner=<当前用户>` 的会话级后端
   （socket 建在 `$XDG_RUNTIME_DIR`，属主是当前用户、权限 `0600`；前端退出后端即结束）。

pkexec 的授权靠 polkit 动作里的 `exec.path` 精确匹配，因此：启动后端命中我们自己的
`org.ranarch.daemon.launch`（认证框是品牌文案）；而首次安装那一次跑的是 AppImage
挂载点里的脚本，匹配不到我们的动作，走系统通用的 `org.freedesktop.policykit.exec`。

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

打包前端（**会把后端一起打进去**，产出 `ui/release/RanArch-Installer-<版本>-x86_64.AppImage`）：

```bash
cd ui && npm install && npm run dist:appimage
```

`npm run dist:appimage` = 整理后端到 `ui/backend/` → 构建渲染层与主进程 → electron-builder 出 AppImage。
**不要**直接 `npx electron-builder`，那样打出来的 AppImage 里没有后端。
（`npm run dist` 会额外尝试打 deb，见下方「已知限制」。）

## 安装系统组件（可选）

**正常情况下不需要这一步** —— 直接跑 AppImage，它会自己在首次运行时把随包的后端装进
`/usr`（弹一次 pkexec 认证框）。想手动做等价的事：

```bash
bash scripts/stage-backend.sh              # 整理后端到 ui/backend/
sudo bash ui/backend/install-backend.sh    # 装进 /usr（幂等，可重复跑）
```

`install-backend.sh` 会装守护进程 / CLI / `libranarch.so`、polkit 策略、systemd unit，
创建 `ranarch` 系统组与运行期目录，并在 `/etc/ranarch/ranarch.conf` 缺失时生成它。
**它不 enable 常驻服务**：前端走的是会话级后端，常驻是另一条路，要开自己开：

```bash
sudo systemctl enable --now ranarch-daemon
```

也可以走 CMake 自己的安装规则（不含运行期目录与系统组）：

```bash
sudo cmake --install build
```

## 用法

图形界面：双击桌面快捷方式，或直接运行 AppImage。把 `.deb` / `.rpm` 拖进中间的六边形，点安装；装包前会弹 polkit 认证框。

第一次运行会连后端一起准备：先弹一次框把后端装进 `/usr`，再弹一次框拉起会话级后端。
之后每次启动弹一次（拉后端），关掉窗口后端跟着结束。想一点系统目录都不动，就先
`sudo bash ui/backend/install-backend.sh` 装好，或者直接改用常驻服务。

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
- **每次启动都要授权一次**。KDE 把应用放在 `app-*.scope` 里，这在 logind 看来不属于任何会话（`logind.GetSessionByPID` 直接报 not in any session），polkit 只能套用动作的 `allow_any` 规则，认证结果也就没法按会话缓存。同理，首次安装后端那一次走的是系统通用动作，认证框文案是通用的。
- 前端 `npm run dist` 里的 deb 打包目标需要 `libxcrypt-compat`（electron-builder 自带的 fpm 依赖 `libcrypt.so.1`）。只出 AppImage 用 `npm run dist:appimage` 即可，这也是推荐方式。

## 许可

MIT，见 [LICENSE](LICENSE)；各源码文件头部有对应的 SPDX 标识。

打包进 AppImage 的自托管字体（Noto Sans SC、DotGothic16、Press Start 2P）各自遵循
SIL Open Font License 1.1，不随本项目的 MIT 授权。另外 AppImage 内含 Electron /
Chromium 及其第三方组件，其许可证见构建产物里的 `LICENSES.chromium.html`。
