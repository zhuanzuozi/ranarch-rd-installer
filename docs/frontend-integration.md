# RanArch RD Installer — 前端接入文档

本文档描述前端（GUI / TUI / 任意语言）如何通过 IPC 调用 `ranarch-daemon`，
完成 `.deb` / `.rpm` 包的查询、安装、卸载。

- 目标读者：前端开发者
- 对应后端版本：`ranarch` 0.1.0
- 协议实现：[protocol.h](../src/daemon/protocol.h)、[ipc_server.cpp](../src/daemon/ipc_server.cpp)

---

## 1. 架构总览

```
┌──────────────┐   Unix socket    ┌──────────────────┐
│  前端 / CLI   │ ───────────────► │  ranarch-daemon  │
│ (ranarchctl) │  NDJSON over UDS │   (root 运行)     │
└──────────────┘ ◄─────────────── └──────────────────┘
                  事件流 / 响应            │
                                           ├─ libalpm  查询已装包
                                           ├─ SQLite   跟踪已装包
                                           ├─ gpgme    验签
                                           └─ libarchive 流式解包
```

前端**不需要** root 权限，也不需要链接 `libranarch`。所有特权操作由
`ranarch-daemon` 完成。

- 默认 socket：`/run/ranarch/ranarch.sock`
- 可配置：`/etc/ranarch/ranarch.conf` 的 `[paths] socket_path`
- 该 socket 由 daemon（root）创建，前端需有读写权限

---

## 2. 通信协议

### 2.1 帧格式

每条消息 = **4 字节大端长度前缀** + **UTF-8 JSON 正文**。

```
┌────────────┬─────────────────────────┐
│ len (u32BE)│  JSON payload (len 字节) │
└────────────┴─────────────────────────┘
```

- 长度是**正文**字节数，不含 4 字节前缀本身
- 单条消息上限 **10 MB**（超出会断开连接）
- 请求与事件都用同一格式，双向复用

### 2.2 请求类型

| `type` | 作用 | 是否长连接 |
|---|---|---|
| `list` | 列出 ranarch 已安装的所有包 | 单次响应 |
| `info` | 查询某个会话的状态与事件回放 | 单次响应 |
| `install` | 安装一个 `.deb`/`.rpm` | **流式**，多事件直到 `done` |
| `answer` | 回答一次 `prompt`（仅安装过程中） | 无独立响应 |
| `remove` | 卸载一个 ranarch 管理的包 | 流式，直到 `done` |

未知 `type` 会返回 `{"type":"error","code":"IPC_PROTOCOL",...}`。

> **注意**：`answer` 必须发在**同一条连接**上，且要在收到 `prompt` 之后立即发。
> daemon 在等待 `answer` 期间会阻塞该次安装（最长 5 分钟）。

---

## 3. 请求详解

### 3.1 `list`

```json
{ "type": "list" }
```

响应：

```json
{
  "type": "packages",
  "packages": [
    {
      "id": 3,
      "name": "trae-cn",
      "version": "1.107.1-1789889373.el8",
      "source_format": "rpm",
      "original_file": "/home/u/下载/TraeCode_CN-linux-x64.rpm",
      "signature_status": "signed_ok",
      "install_date": 1758800000
    }
  ]
}
```

- `source_format`：`"deb"` / `"rpm"`
- `signature_status`：`"signed_ok"` / `"unsigned"` / `"bad"` / `"skipped"`
- `install_date`：Unix 秒
- 注意：这里只列 **ranarch 安装的包**，不含 pacman 的包

### 3.2 `install`

```json
{
  "type": "install",
  "path": "/home/u/下载/foo.rpm",
  "options": {
    "interactive": true,
    "dry_run": false,
    "checks": {
      "signature": true,
      "dependencies": true,
      "conflicts": true,
      "path_traversal": true,
      "sandbox_trial": false
    }
  }
}
```

`options` 与 `options.checks` 都可整体省略，省略时使用 daemon 配置
（`/etc/ranarch/ranarch.conf` 的 `[checks]` 段）的默认值。

安装是**流式**的：收到请求后 daemon 先回一条 `ok`（含 `session_id`），
然后持续推送事件，最后一条必为 `done`。

### 3.3 `remove`

```json
{ "type": "remove", "package_id": 3 }
```

`package_id` 取自 `list` 响应。删除 ranarch 记录的文件与数据库行。

### 3.4 `info`

```json
{ "type": "info", "session_id": "1679e356f4d8-1" }
```

响应（用于「重连后重放」或「查看历史安装」）：

```json
{
  "type": "session",
  "session_id": "1679e356f4d8-1",
  "file_path": "/home/u/下载/foo.rpm",
  "active": false,
  "done": true,
  "events": [ "{\"type\":\"step\",\"step\":\"parsing\"}", "..." ],
  "ok": true,
  "summary": "dry-run successful for foo",
  "installed_files_count": 0
}
```

`events` 是**字符串数组**（每个元素本身是一段 JSON 文本），需要二次解析。
会话不存在时返回 `IPC_SESSION_NOT_FOUND`。

---

## 4. 响应与事件

安装/卸载过程中前端会依次收到：

### 4.1 `ok` — 会话已建立

```json
{ "type": "ok", "session_id": "1679d238866a-0" }
```

### 4.2 `checks_disabled` — 有校验被关闭

```json
{ "type": "checks_disabled",
  "checks": ["signature", "dependencies", "conflicts", "path_traversal"] }
```

**前端必须显著提示用户**：这是一次刻意绕过安全防护的安装。
事件总在第一个 `step` 之前发出。

### 4.3 `step` — 阶段推进

```json
{ "type": "step", "step": "parsing", "detail": "/home/u/下载/foo.rpm" }
```

`step` 取值（按正常顺序）：

| step | 含义 |
|---|---|
| `parsing` | 解析包元数据（只读头部，不解压） |
| `verifying_sig` | GPG 签名验证 |
| `resolving_deps` | 依赖解析 |
| `conflict_check` | 文件冲突检测 |
| `sandbox_trial` | 沙箱试装（`dry_run` 或 `sandbox_trial` 开启时） |
| `staging` | 写入暂存 / 落盘 |
| `installing` | 安装中（detail 含文件数） |
| `recording` | 写入 SQLite 跟踪库 |
| `done` | 结束（detail 为结果摘要） |

`detail` 可能为空字符串。

### 4.4 `done` — 最终结果（终止事件）

成功：

```json
{ "type": "done", "ok": true, "summary": "installed tree-1.8.0-10.el9 (9 files)",
  "installed_files_count": 9 }
```

失败：

```json
{ "type": "done", "ok": false, "summary": "installing /path/foo.rpm",
  "installed_files_count": 0,
  "error_code": "EXTRACT_PERMISSION",
  "error_message": "open failed: /usr/bin/tree: Permission denied" }
```

**约定**：`done` 之后本次请求结束，前端应关闭或复用连接。

### 4.5 `prompt` — 需要用户决策（`interactive: true` 时）

当某个依赖没被满足（未映射，或映射到了但系统没装），daemon 会发一条
`prompt` 并**阻塞等待** `answer`：

```json
{
  "type": "prompt",
  "session_id": "16a648679723-1",
  "prompt_id": 1,
  "kind": "dependency",
  "dependency": {
    "raw_name": "libc.so.6()(64bit)",
    "arch_candidate": "",
    "op": "",
    "version": "",
    "mapped": false
  },
  "options": ["install", "skip", "map", "abort"]
}
```

- `arch_candidate` 为空 = 映射表里没有对应 Arch 包（`mapped: false`）
- `options` 恒为四项；前端应据 `mapped` 决定文案：
  - `mapped: true` 时 `install` 可直接用 `arch_candidate`
  - `mapped: false` 时必须让用户**输入** Arch 包名

前端须在同一条连接回 `answer`：

```json
{ "type": "answer", "session_id": "16a648679723-1",
  "prompt_id": 1, "choice": "map", "arch_name": "glibc" }
```

| `choice` | 语义 | 需要 `arch_name` |
|---|---|---|
| `install` | 立刻 `pacman -S <arch_name>`，并流式回传输出 | 是 |
| `skip` | 跳过该依赖继续安装 | 否 |
| `map` | 记录 `<raw_name> → <arch_name>` 映射并持久化，不安装 | 是 |
| `abort` | 中止本次安装 | 否 |

`map` 会写入数据库的 `dep_overrides`，**下次安装同一包不会再问这一项**。

若 5 分钟内没有 `answer`（或连接断开），daemon 按 `abort` 处理并记 WARN 日志。

### 4.6 `pty_start` / `pty_data` / `pty_end` — 子进程实时终端

当用户选择 `install`，daemon 通过 PTY 执行 `pacman -S`，输出实时回传：

```json
{ "type": "pty_start", "cmd": "pacman -S glibc" }
{ "type": "pty_data", "data": "6ZSZ6K+v77ya6Z2eIHJvb3Qg..." }
{ "type": "pty_end", "exit_code": 1 }
```

**`pty_data.data` 是 base64**，不是明文 —— 终端输出含 ANSI 转义与半截 UTF-8
字节，不能直接放进 JSON 字符串。前端应：

1. base64 解码成字节
2. 直接写进终端模拟器 / `<pre>`（浏览器可 `atob` 后按 latin1 或 TextDecoder 流式解码）

前端可以不做终端渲染，仅提示「正在安装依赖」并显示 `exit_code`。

### 4.7 `error` — 协议层错误

只用于「请求本身不合法」，与安装失败区分开：

```json
{ "type": "error", "code": "IPC_PROTOCOL", "message": "unknown request type: foo" }
```

---

## 4.8 完整事件序列示例

一次「跳过全部依赖」的 dry-run：

```
{"type":"ok","session_id":"169d68967b22-0"}
{"type":"checks_disabled","checks":["signature"]}
{"type":"step","step":"parsing","detail":"/path/tree.rpm"}
{"type":"step","step":"verifying_sig"}
{"type":"step","step":"resolving_deps"}
{"type":"prompt","prompt_id":1,...}
   ← 前端发 answer(choice=skip)
{"type":"step","step":"conflict_check"}
{"type":"step","step":"sandbox_trial"}
{"type":"step","step":"done","detail":"dry-run ok"}
{"type":"done","ok":true,"summary":"dry-run successful for tree","installed_files_count":0}
```

一次「安装依赖」的片段：

```
{"type":"step","step":"dep_install","detail":"glibc"}
{"type":"pty_start","cmd":"pacman -S glibc"}
{"type":"pty_data","data":"..."}
{"type":"pty_end","exit_code":0}
```

---

## 5. 错误码对照表

`error_code` 与 `error.code` 使用同一套稳定字符串。**数值永不改变**，
前端应以字符串为准做本地化。

| 码 | 中文含义 | 典型触发 |
|---|---|---|
| `OK` | 成功 | — |
| `PARSE_BAD_MAGIC` | 不是有效的 .deb/.rpm | 文件损坏或扩展名不符 |
| `PARSE_TRUNCATED` | 包文件被截断 | 下载未完成 |
| `PARSE_UNSUPPORTED_COMPRESSION` | 不支持的压缩算法 | — |
| `PARSE_BAD_HEADER` | 包头格式错误 | 非标准打包工具产物 |
| `CONFLICT_PACMAN` | 与 pacman 已装包文件冲突 | 目标文件已被 pacman 包占用 |
| `CONFLICT_RANARCH` | 与其他 ranarch 包冲突 | 同一路径已被 ranarch 装过 |
| `CONFLICT_FS` | 与文件系统已有文件冲突 | — |
| `DEP_UNMAPPED` | 依赖名无对应 Arch 映射 | deb/rpm 依赖名不在映射表 |
| `DEP_UNRESOLVED` | 依赖无法解析 | — |
| `DEP_INSTALL_FAILED` | 依赖安装失败 | pacman 安装依赖出错 |
| `EXTRACT_DISK_FULL` | 磁盘空间不足 | — |
| `EXTRACT_PERMISSION` | 写入目标文件权限不足 | 非 root 安装到 `/usr` |
| `EXTRACT_PATH_TRAVERSAL` | 路径越出安装根目录（已拦截） | 恶意/异常包 |
| `SIGNATURE_BAD` | 签名验证失败 | 包被篡改或密钥不匹配 |
| `SIGNATURE_MISSING` | 包未签名，但策略要求签名 | `policy = strict` |
| `SIGNATURE_KEY_MISSING` | 签名密钥不在信任钥匙环 | 需先导入公钥 |
| `SANDBOX_UNAVAILABLE` | 沙箱后端不可用 | — |
| `SANDBOX_TRIAL_FAILED` | 沙箱试装失败 | dry-run 发现问题 |
| `DB_CORRUPT` | 跟踪数据库损坏 | — |
| `DB_LOCKED` | 数据库被占用 | — |
| `IPC_AUTH` | 无权执行该操作 | — |
| `IPC_PROTOCOL` | IPC 协议错误 | 请求 type 未知 / 帧损坏 |
| `IPC_SESSION_NOT_FOUND` | 会话不存在 | `info` 传了错误 id |
| `INTERNAL` | 内部错误 | 兜底 |
| `IO` | 输入输出错误 | 文件打不开等 |
| `CANCELLED` | 操作被取消 | — |
| `BLOCKED_BY_CONFLICT` | 因文件冲突被阻止 | `error_message` 含冲突数 |
| `NEEDS_USER_INPUT` | 需要用户输入才能继续 | 依赖未映射待确认 |

> 与 [ui/src/errors.ts](../ui/src/errors.ts) 保持一致；新增码时两边同步。

---

## 6. 校验收开关

每个校验都可以**逐项关闭**。四层入口，优先级从高到低：

| 层 | 位置 | 说明 |
|---|---|---|
| 1 | IPC `options.checks` | 前端按次覆盖 |
| 2 | CLI `--no-*` | 命令行 |
| 3 | `/etc/ranarch/ranarch.conf` `[checks]` | daemon 全局默认 |
| 4 | 代码默认值 | 全部开启 |

| 开关 | 默认 | 关闭后的后果 |
|---|---|---|
| `signature` | `true` | 不验签，记录为 `skipped`。等价 `--no-signature` |
| `dependencies` | `true` | 不解析依赖，等价 pacman `--nodeps` |
| `conflicts` | `true` | **允许覆盖**其他包的文件，等价 `--overwrite` |
| `path_traversal` | `true` | **危险**：不再拦截越出根目录的路径，等价于放弃对恶意包的最后一道防线 |
| `sandbox_trial` | `false` | 这是「额外开启」的项，开启则落盘前先试装 |

规则：

- 关闭任一项，daemon 都会发一条 `checks_disabled` 事件并写 WARN 日志
- 未在 `checks` 里出现的键，沿用 daemon 配置默认值
- 前端至少要对 `path_traversal` 的关闭做二次确认

配置文件示例：

```ini
[checks]
signature = true
dependencies = true
conflicts = true
path_traversal = true
sandbox_trial = false
```

---

## 7. TypeScript 接入示例

```ts
import net from "node:net";

const SOCKET = "/run/ranarch/ranarch.sock";

/** 与 daemon 的一条长连接。协议：4 字节大端长度 + JSON 正文。 */
class RanArchClient {
  private sock: net.Socket;
  private buf = Buffer.alloc(0);
  private onMsg: (msg: any) => void = () => {};

  constructor() {
    this.sock = net.connect(SOCKET);
    this.sock.on("data", (chunk) => {
      this.buf = Buffer.concat([this.buf, chunk]);
      // 循环取出完整帧
      while (this.buf.length >= 4) {
        const len = this.buf.readUInt32BE(0);
        if (this.buf.length < 4 + len) break;
        const body = this.buf.subarray(4, 4 + len).toString("utf8");
        this.buf = this.buf.subarray(4 + len);
        this.onMsg(JSON.parse(body));
      }
    });
  }

  private send(obj: unknown) {
    const body = Buffer.from(JSON.stringify(obj), "utf8");
    const head = Buffer.alloc(4);
    head.writeUInt32BE(body.length, 0);
    this.sock.write(Buffer.concat([head, body]));
  }

  list(): Promise<any> {
    return new Promise((resolve) => {
      this.onMsg = (m) => { if (m.type === "packages") resolve(m.packages); };
      this.send({ type: "list" });
    });
  }

  /** 安装并流式回调每个事件；resolve 于 done。 */
  install(
    path: string,
    opts: {
      dryRun?: boolean;
      checks?: Record<string, boolean>;
      /** 回答依赖问题；返回选择。不提供则一律 skip。 */
      onDependency?: (dep: any) => {
        choice: "install" | "skip" | "map" | "abort";
        archName?: string;
      };
      onPtyData?: (bytes: Buffer) => void;
    } = {},
    onEvent?: (ev: any) => void,
  ): Promise<{ ok: boolean; sessionId?: string; errorCode?: string }> {
    return new Promise((resolve, reject) => {
      let sessionId: string | undefined;
      this.onMsg = (ev) => {
        onEvent?.(ev);
        switch (ev.type) {
          case "ok":
            sessionId = ev.session_id;
            break;
          case "checks_disabled":
            // 前端应在此显著警告用户
            console.warn("[ranarch] 校验被关闭:", ev.checks.join(", "));
            break;
          case "prompt": {
            // 必须立刻回答，daemon 正在阻塞等待
            const ans = opts.onDependency?.(ev.dependency) ?? { choice: "skip" };
            this.send({
              type: "answer",
              session_id: ev.session_id,
              prompt_id: ev.prompt_id,
              choice: ans.choice,
              arch_name: ans.archName ?? "",
            });
            break;
          }
          case "pty_start":
            console.log(`$ ${ev.cmd}`);
            break;
          case "pty_data":
            // data 是 base64：终端字节流，不能当普通字符串处理
            opts.onPtyData?.(Buffer.from(ev.data, "base64"));
            break;
          case "pty_end":
            console.log(`(exit ${ev.exit_code})`);
            break;
          case "error":
            reject(new Error(`${ev.code}: ${ev.message}`));
            break;
          case "done":
            resolve({ ok: ev.ok, sessionId, errorCode: ev.error_code });
            break;
        }
      };
      this.send({
        type: "install",
        path,
        options: {
          interactive: true,
          dry_run: opts.dryRun ?? false,
          checks: opts.checks ?? {},
        },
      });
    });
  }

  close() { this.sock.end(); }
}

// ---- 用法 ----
const c = new RanArchClient();
const pkgs = await c.list();
console.log("已安装:", pkgs.map((p: any) => `${p.name} ${p.version}`));

const r = await c.install(
  "/home/u/下载/tree.rpm",
  {
    // 依赖决策：可以在这里弹 UI 让用户选
    onDependency: (dep) => {
      if (dep.mapped) {
        if (dep.arch_candidate === "glibc") {
          return { choice: "skip" }; // 系统本来就有，跳过即可
        }
        return { choice: "install", archName: dep.arch_candidate };
      }
      // 无映射：需要用户输入 Arch 包名
      return { choice: "map", archName: "glibc" };
    },
    onPtyData: (bytes) => process.stdout.write(bytes),
  },
  (ev) => {
    if (ev.type === "step") console.log(`[${ev.step}] ${ev.detail ?? ""}`);
  },
);
console.log(r.ok ? "安装成功" : `失败: ${r.errorCode}`);
c.close();
```

---

## 8. CLI 参考（同一协议）

```bash
# 列出已安装
ranarchctl list

# 安装（默认所有校验开启）
ranarchctl install ./foo.rpm

# 只试装，不落盘
ranarchctl install ./foo.rpm --dry-run

# 落盘前额外跑一次沙箱试装
ranarchctl install ./foo.rpm --sandbox

# 逐项关闭校验
ranarchctl install ./foo.rpm --no-signature
ranarchctl install ./foo.rpm --no-dependencies
ranarchctl install ./foo.rpm --no-conflicts
ranarchctl install ./foo.rpm --no-path-traversal   # 危险

# 一次性关闭全部（等价 --force）
ranarchctl install ./foo.rpm --force

# 卸载 / 查询会话
ranarchctl remove 3
ranarchctl info 1679e356f4d8-1

# 非默认 socket
ranarchctl list --socket /tmp/ranarch-e2e/ranarch.sock
```

退出码：`0` 成功，`1` 失败（含参数错误、连接失败、安装失败）。

---

## 9. 当前限制（前端需知）

1. **安装是同步的**：daemon 单线程处理请求，一个安装进行中会阻塞其他客户端。
   前端应自行串行化并显示「等待中」。
2. **无取消接口**：安装一旦开始无法中途取消（`CANCELLED` 码已预留）。
3. **`interactive: false` 时不会发 `prompt`**：未满足的依赖按
   `ranarch.conf` 的 `[deps] default_action`（`ask` / `skip` / `abort`）处理，
   `ask` 在无交互通道时退化为 `skip`。纯脚本化安装请显式传 `interactive: false`
   以免 daemon 等待答复。
4. **pacman 用 `--noconfirm` 执行**：用户已在 `prompt` 里确认过，因此不再让
   pacman 二次询问（冲突等场景 pacman 会自选默认项）。
5. **一次只回答一个问题**：`prompt` 是串行的，前端不必做队列。
6. **polkit 授权未启用**：本机未安装 `polkit-gobject-2` 时，daemon 编译期不含
   授权层；生产环境需确保 socket 权限或补上 polkit 支持。
7. **签名策略**：`strict` 会拒绝未签名包；`warn`（默认）放行但记 WARN；
   `ignore` 一律记 `skipped`。策略在 `ranarch.conf` 的 `[signature] policy`。

---

## 10. 大包行为说明

解析器**不把包读进内存**：只读头部（RPM 约 10 KB），解包时从包文件直接流式写入磁盘。

实测（TraeCode 595 MB RPM，8650 个文件）：

| 指标 | 重构前 | 重构后 |
|---|---|---|
| 解析峰值内存 | 2326 MB | **20 MB** |
| 解包峰值内存 | — | **20 MB** |
| 解包耗时 | — | 约 5.4 秒 |

因此几百 MB 的包（IDE、游戏、工具链）前端可以直接安装，无需特殊处理。
