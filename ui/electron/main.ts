// RanArch Installer — Electron 主进程
//
// 职责：
//   1. 作为 IPC 客户端连接 ranarch-daemon（Unix socket，4 字节大端长度前缀 + JSON）
//   2. 把守护进程的事件流转发给所有窗口
//   3. 请求/响应关联、断线自动重连
//   4. 维护三个恒定尺寸的窗口：主窗 600×600、队列面板 400×600、终端面板 宽×410
//
// 「积木」架构：主窗永不改尺寸；队列贴在主窗右侧、终端贴在主窗下方，共享边精确
// 贴齐（零间隙、零重叠），三块拼起来是一个完整矩形，互不挤压：
//
//     ┌──────────┬────────┐
//     │  主窗    │  队列  │     主窗 600×600（恒定不变）
//     │ 600×600  │ 400×600│     队列 400×600：贴主窗右侧
//     ├──────────┴────────┤     终端 600×410：贴主窗下方
//     │   终端 600×410     │     打开「终端横跨队列」后才变成 1000 宽铺满下面一行
//     └───────────────────┘
//
// 面板的出现 / 消失：窗口透明、坐标固定，动画全在面板窗里用 CSS 位移完成。
//   面板窗口正好贴在共享边上（队列在主窗右侧、终端在主窗下方），窗口本身透明，
//   内容初始停在「主窗身位之外」（队列 translateX(-100%)、终端 translateY(-100%)）——
//   被窗口裁掉，什么都看不到；把内容滑到 0，面板就像从主窗边缘被抽出来一样出现。
//   重叠在主窗上的那一段永远在窗口之外被裁掉，所以面板绝不会叠在主界面上面；
//   窗口全程不动、不改尺寸、不改透明度，因此也没有逐帧抖动、黑线或残影。
//   唯一的窗口尺寸变化是终端高度的可用空间自适应（见 panelSize）。
//
// 后端（ranarch-daemon）的启动由本进程负责，见下方「后端生命周期」一节：
// 若系统守护进程已在跑就直接连；否则必要时先一次性把它装进 /usr，
// 再用 pkexec 拉起一个「前端退出它就退出」的会话级实例。
import { app, BrowserWindow, ipcMain, dialog, screen } from 'electron';
import { createConnection, Socket } from 'net';
import { spawn } from 'child_process';
import path from 'path';
import fs from 'fs';

// ---------- 常量 ----------

const DEFAULT_SOCKET = '/run/ranarch/ranarch.sock';
const DEFAULT_CONFIG = '/etc/ranarch/ranarch.conf';
const MAX_FRAME = 10 * 1024 * 1024; // 与后端 recv_message 的 sanity 上限一致

// ---------- 后端路径 ----------
//
// 系统守护进程走 /run/ranarch/ranarch.sock（root:ranarch 0750，需要用户在
// ranarch 组里）；会话级后端把 socket 建在用户的 runtime 目录下，属主是当前
// 用户、权限 0600，只有本人能连。

/** 随包后端产物的目录：打包后是 resources/backend，开发时是仓库里的 ui/backend */
const BACKEND_DIR = app.isPackaged
  ? path.join(process.resourcesPath, 'backend')
  : path.join(__dirname, '../backend');

const MY_UID = process.getuid?.() ?? 0;

function runtimeDir(): string {
  return process.env.XDG_RUNTIME_DIR || `/tmp/ranarch-session-${MY_UID}`;
}

const SESSION_SOCK = path.join(runtimeDir(), 'ranarch.sock');
const SESSION_CONF = path.join(runtimeDir(), 'ranarch-session.conf');
const SESSION_LOG = path.join(runtimeDir(), 'ranarch-session.log');

/**
 * socket 是否「真能用」：不只是存在，还得是 socket 且我们可读写。
 * 只看存在会踩坑 —— 比如上一版后端留下的 root:root 0755 的僵尸 socket，
 * 看着在、其实连不上。
 */
function socketUsable(p: string): boolean {
  try {
    if (!fs.statSync(p).isSocket()) return false;
    fs.accessSync(p, fs.constants.R_OK | fs.constants.W_OK);
    return true;
  } catch {
    return false;
  }
}

/**
 * 后端启动过程中的当前动作（如「首次运行：正在安装后端…」）。
 *
 * 连接状态广播里会把它当作 detail 带上：客户端自己的状态变化只会说
 * 「connecting / unable to connect」，而拉起后端这段时间恰恰是最需要
 * 告诉用户「它在干什么、为什么弹了认证框」的时候。
 */
let backendNote = '';

type ConnState = 'connecting' | 'connected' | 'disconnected';

interface CheckToggles {
  signature: boolean;
  dependencies: boolean;
  conflicts: boolean;
  path_traversal: boolean;
  sandbox_trial: boolean;
}

interface DaemonPolicy {
  loaded: boolean;
  configPath: string;
  checks: CheckToggles;
  signature_policy: 'strict' | 'warn' | 'ignore';
  sandbox_trial_install: boolean;
  sandbox_backend: 'bwrap' | 'nspawn' | 'none';
  dep_default_action: 'ask' | 'skip' | 'abort';
}

// ---------- 工具 ----------

function formatSize(bytes: number): string {
  if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + ' MB';
  if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return bytes + ' B';
}

/** 极简 INI 解析：返回 [section][key] = value。行首 # / ; 为注释，行内 # 之后丢弃。 */
function parseIni(text: string): Record<string, Record<string, string>> {
  const out: Record<string, Record<string, string>> = {};
  let section = '';
  for (const raw of text.split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#') || line.startsWith(';')) continue;
    if (line.startsWith('[')) {
      const end = line.indexOf(']');
      section = end > 0 ? line.slice(1, end).trim() : '';
      continue;
    }
    const eq = line.indexOf('=');
    if (eq < 0 || !section) continue;
    const key = line.slice(0, eq).trim();
    const val = line.slice(eq + 1).split('#')[0].trim();
    if (key) (out[section] ??= {})[key] = val;
  }
  return out;
}

function parseBool(v: string | undefined, dflt: boolean): boolean {
  if (v === undefined) return dflt;
  const s = v.toLowerCase();
  return s === 'true' || s === '1' || s === 'yes' || s === 'on';
}

function oneOf<T extends string>(v: string | undefined, allowed: readonly T[], dflt: T): T {
  const s = (v ?? '').toLowerCase();
  return (allowed as readonly string[]).includes(s) ? (s as T) : dflt;
}

/**
 * 配置文件路径，优先级：
 *   1. RANARCH_CONFIG 环境变量（开发/测试）
 *   2. 系统守护进程可用 → /etc/ranarch/ranarch.conf
 *   3. 否则 → 本会话的会话级配置（由 writeSessionConfig 生成，与会话级后端一致）
 */
function resolveConfigPath(): string {
  if (process.env.RANARCH_CONFIG) return process.env.RANARCH_CONFIG;
  return socketUsable(DEFAULT_SOCKET) ? DEFAULT_CONFIG : SESSION_CONF;
}

// ---------- 系统占用（状态栏 CPU / 内存） ----------

interface SystemUsage {
  /** CPU 使用率 0–100（两次 /proc/stat 采样差值） */
  cpu: number;
  mem: {
    /** 内存占用率 0–100 */
    percent: number;
    usedMb: number;
    totalMb: number;
  };
}

/** 上一次 /proc/stat 采样；CPU 使用率必须靠两次采样的差值算 */
let lastCpuSample: { total: number; idle: number } | null = null;

/** 读 /proc/stat 第一行（整机汇总）→ 总时间与空闲时间 */
function readCpuSample(): { total: number; idle: number } {
  const line = fs.readFileSync('/proc/stat', 'utf8').split('\n', 1)[0];
  const parts = line.trim().split(/\s+/).slice(1).map(Number);
  const idle = (parts[3] ?? 0) + (parts[4] ?? 0); // idle + iowait
  return { total: parts.reduce((a, b) => a + b, 0), idle };
}

/** 读 /proc/meminfo：占用率按 MemTotal - MemAvailable 算（与 free -m 的口径一致） */
function readMemory(): SystemUsage['mem'] {
  const text = fs.readFileSync('/proc/meminfo', 'utf8');
  const field = (name: string) =>
    Number(new RegExp(`^${name}:\\s+(\\d+)`, 'm').exec(text)?.[1] ?? 0);
  const totalKb = field('MemTotal');
  const usedKb = Math.max(0, totalKb - field('MemAvailable'));
  return {
    percent: totalKb ? Math.round((usedKb / totalKb) * 100) : 0,
    usedMb: Math.round(usedKb / 1024),
    totalMb: Math.round(totalKb / 1024),
  };
}

/**
 * 系统占用快照。
 * 第一次调用还没有上一次采样做基准，就地补一次短间隔采样（150ms），
 * 保证任何一次调用返回的都是真实数字，而不是 0 或占位值。
 */
async function readSystemUsage(): Promise<SystemUsage> {
  let prev = lastCpuSample;
  if (!prev) {
    prev = readCpuSample();
    await new Promise(r => setTimeout(r, 150));
  }
  const now = readCpuSample();
  lastCpuSample = now;
  const totalDelta = now.total - prev.total;
  const idleDelta = now.idle - prev.idle;
  const cpu = totalDelta > 0
    ? Math.min(100, Math.max(0, Math.round((1 - idleDelta / totalDelta) * 100)))
    : 0;
  return { cpu, mem: readMemory() };
}

/**
 * 读取守护进程策略。
 *
 * 这是修正「配置里关了校验、界面上却显示开启」的关键：设置面板里的策略项
 * 必须来自真实配置，而不是前端写死的编译期默认值。
 * 对应后端 [checks] / [signature] / [sandbox] / [deps] 四个配置段。
 */
function readDaemonPolicy(): DaemonPolicy {
  const configPath = resolveConfigPath();
  const fallback: DaemonPolicy = {
    loaded: false,
    configPath,
    checks: { signature: true, dependencies: true, conflicts: true, path_traversal: true, sandbox_trial: false },
    signature_policy: 'warn',
    sandbox_trial_install: true,
    sandbox_backend: 'bwrap',
    dep_default_action: 'ask',
  };
  try {
    const ini = parseIni(fs.readFileSync(configPath, 'utf-8'));
    const c = ini.checks ?? {};
    const s = ini.sandbox ?? {};
    const d = ini.deps ?? {};
    return {
      loaded: true,
      configPath,
      // ValidationOptions 的代码默认值：四项开启，sandbox_trial 为 opt-in 默认关闭
      checks: {
        signature: parseBool(c.signature, true),
        dependencies: parseBool(c.dependencies, true),
        conflicts: parseBool(c.conflicts, true),
        path_traversal: parseBool(c.path_traversal, true),
        sandbox_trial: parseBool(c.sandbox_trial, false),
      },
      signature_policy: oneOf(ini.signature?.policy, ['strict', 'warn', 'ignore'] as const, 'warn'),
      sandbox_trial_install: parseBool(s.trial_install, true),
      sandbox_backend: oneOf(s.backend, ['bwrap', 'nspawn', 'none'] as const, 'bwrap'),
      dep_default_action: oneOf(d.default_action, ['ask', 'skip', 'abort'] as const, 'ask'),
    };
  } catch {
    return fallback;
  }
}

/**
 * 解析 socket 路径，优先级：
 *   1. RANARCH_SOCKET 环境变量（开发/测试）
 *   2. 系统配置里 [paths] socket_path 指向的 socket（默认 /run/ranarch/ranarch.sock）
 *      —— 只有它真能连上才算数
 *   3. 否则 → 会话级后端的 socket（由 ensureBackend 负责把后端拉起来）
 */
function resolveSocketPath(): string {
  if (process.env.RANARCH_SOCKET) return process.env.RANARCH_SOCKET;
  try {
    const ini = parseIni(fs.readFileSync(DEFAULT_CONFIG, 'utf-8'));
    const v = ini.paths?.socket_path;
    if (v && socketUsable(v)) return v;
  } catch {
    // 配置不存在或不可读 —— 退到默认路径判断
  }
  if (socketUsable(DEFAULT_SOCKET)) return DEFAULT_SOCKET;
  return SESSION_SOCK;
}

// ---------- IPC 客户端 ----------

interface Pending {
  expect: Set<string>;
  resolve: (msg: any) => void;
  reject: (err: Error) => void;
  timer: NodeJS.Timeout;
}

class IpcClient {
  private sock: Socket | null = null;
  private buf = Buffer.alloc(0);
  private state: ConnState = 'disconnected';
  private lastError = '';
  private pending: Pending[] = [];
  private reconnectDelay = 500;
  private stopped = false;

  constructor(
    private readonly socketPath: string,
    /** 收到任意后端消息时回调（转发给渲染进程） */
    private readonly onMessage: (msg: any) => void,
    /** 连接状态变化时回调 */
    private readonly onStateChange: (state: ConnState, detail: string) => void,
  ) {}

  start(): void {
    this.connect();
  }

  stop(): void {
    this.stopped = true;
    this.sock?.destroy();
    this.sock = null;
    this.rejectAll(new Error('客户端已关闭'));
  }

  getState(): { state: ConnState; socketPath: string; error: string } {
    return { state: this.state, socketPath: this.socketPath, error: this.lastError };
  }

  /** 发送请求并等待指定类型的响应（用于需要回执的请求） */
  request(msg: object, expectTypes: string[], timeoutMs = 20000): Promise<any> {
    return new Promise((resolve, reject) => {
      if (!this.sock) {
        reject(new Error(`未连接到守护进程 (${this.socketPath})`));
        return;
      }
      const pending: Pending = {
        expect: new Set(expectTypes),
        resolve,
        reject,
        timer: setTimeout(() => {
          this.pending = this.pending.filter(p => p !== pending);
          reject(new Error(`等待响应超时: ${expectTypes.join('/')}`));
        }, timeoutMs),
      };
      this.pending.push(pending);
      this.write(msg);
    });
  }

  /** 发送请求但不等待回执（响应以事件流形式到达） */
  send(msg: object): void {
    if (!this.sock) throw new Error(`未连接到守护进程 (${this.socketPath})`);
    this.write(msg);
  }

  // ---- 内部实现 ----

  private connect(): void {
    if (this.stopped) return;
    this.setState('connecting', '');

    const sock = createConnection(this.socketPath);
    this.sock = sock;

    sock.on('connect', () => {
      this.reconnectDelay = 500;
      this.buf = Buffer.alloc(0);
      this.setState('connected', '');
    });

    sock.on('data', (chunk: Buffer) => this.onData(chunk));

    sock.on('error', (err: Error) => {
      this.lastError = err.message;
    });

    sock.on('close', () => {
      const wasConnected = this.state === 'connected';
      this.sock = null;
      this.rejectAll(new Error('与守护进程的连接已断开'));
      this.setState('disconnected', this.lastError || (wasConnected ? '连接已关闭' : '无法连接'));
      this.scheduleReconnect();
    });
  }

  private scheduleReconnect(): void {
    if (this.stopped) return;
    const delay = this.reconnectDelay;
    this.reconnectDelay = Math.min(this.reconnectDelay * 1.6, 8000);
    setTimeout(() => this.connect(), delay);
  }

  private setState(state: ConnState, detail: string): void {
    if (this.state === state && !detail) return;
    this.state = state;
    this.onStateChange(state, detail);
  }

  private write(msg: object): void {
    const payload = Buffer.from(JSON.stringify(msg), 'utf-8');
    const header = Buffer.alloc(4);
    header.writeUInt32BE(payload.length, 0);
    this.sock?.write(Buffer.concat([header, payload]));
  }

  private onData(chunk: Buffer): void {
    this.buf = Buffer.concat([this.buf, chunk]);
    while (this.buf.length >= 4) {
      const len = this.buf.readUInt32BE(0);
      if (len > MAX_FRAME) {
        this.lastError = `帧长度异常: ${len}`;
        this.sock?.destroy();
        return;
      }
      if (this.buf.length < 4 + len) break;
      const json = this.buf.subarray(4, 4 + len).toString('utf-8');
      this.buf = this.buf.subarray(4 + len);

      let msg: any;
      try {
        msg = JSON.parse(json);
      } catch {
        this.lastError = '收到无法解析的消息';
        continue;
      }

      // 1) 先投递给等待中的请求（FIFO 匹配响应类型）
      const idx = this.pending.findIndex(p => p.expect.has(msg.type));
      if (idx >= 0) {
        const [p] = this.pending.splice(idx, 1);
        clearTimeout(p.timer);
        if (msg.type === 'error') {
          p.reject(new Error(msg.message || '后端返回错误'));
        } else {
          p.resolve(msg);
        }
      }

      // 2) 同时作为事件流转发给渲染进程（step / done 等流式事件）
      this.onMessage(msg);
    }
  }

  private rejectAll(err: Error): void {
    const pending = this.pending;
    this.pending = [];
    pending.forEach(p => {
      clearTimeout(p.timer);
      p.reject(err);
    });
  }
}

// ---------- 应用状态 ----------

type PanelName = 'queue' | 'terminal';
const PANEL_NAMES: PanelName[] = ['queue', 'terminal'];

/** 主窗设计尺寸 —— 恒定不变，任何代码都不得改变它 */
const MAIN_W = 600;
const MAIN_H = 600;

/** 面板尺寸恒定的设计值：队列 400×600（贴主窗右侧） */
const QUEUE_W = 400;
const QUEUE_H = 600;
/**
 * 终端设计高度 410，贴在主窗下方。
 * 宽度等于「主区 + 队列」，随队列开合在 600 / 1000 之间切换 —— 两块拼起来才是完整矩形。
 */
const TERMINAL_H = 410;
/**
 * 面板最小尺寸。
 *
 * 主窗被拖到工作区边缘时可用空间可能趋近于 0，这里给个下限，避免出现 0 尺寸窗口。
 */
const PANEL_MIN = 160;

/**
 * 抽出 / 收入动画时长，必须与样式表里的 --duration-panel-out / -in 一致：
 * 主进程据此决定「收入动画播完后多久隐藏窗口」。
 */
const PANEL_OPEN_MS = 260;
const PANEL_CLOSE_MS = 190;
/** 隐藏前的余量：宁可多等两帧，也不要在内容还没剪裁干净时把窗口收掉 */
const PANEL_HIDE_GRACE_MS = 80;

interface PanelState {
  queueOpen: boolean;
  terminalOpen: boolean;
  /** 终端是否横跨到队列下方（开关在选项菜单里，默认关） */
  terminalSpan: boolean;
}

let mainWindow: BrowserWindow | null = null;
const panelWindows: Record<PanelName, BrowserWindow | null> = { queue: null, terminal: null };

/** 面板的逻辑开合状态：主进程是唯一权威，变更后广播给所有窗口（菜单按钮按下态据此同步） */
const panelOpen: Record<PanelName, boolean> = { queue: false, terminal: false };
/** 收入动画播完后的隐藏定时器；为 null 表示没有待执行的隐藏 */
const panelHideTimers: Record<PanelName, NodeJS.Timeout | null> = { queue: null, terminal: null };

/**
 * 装配体让位。
 *
 * 面板要往右 / 往下生长时，若主窗离工作区边界太近，装配体就整体让位：
 * 主窗最多平移工作区顶部 / 左边缘，把空间腾给面板 —— 三块窗口的尺寸因此都能保住，
 * 而不是把面板压扁，更不会让面板压到主界面上
 * （面板 400px 宽 + 410px 高、600 × 1010 的装配体在带任务栏 / 靠边的窗口上常常放不下）。
 *
 * naturalX / naturalY 是主窗「没有让位时」的位置：用户拖窗时跟着更新，让位只是相对它偏移。
 */
let naturalX: number | null = null;
let naturalY: number | null = null;
let assemblyShiftX = 0;
let assemblyShiftY = 0;

/** 终端是否横跨到队列下方（选项菜单里的开关，默认关：终端始终只占主区宽） */
let panelSpan = false;

/** 最近一次队列/进度快照，用于面板窗加载完成后补齐状态 */
let lastSnapshot: unknown = null;

let client: IpcClient | null = null;
let socketPath = DEFAULT_SOCKET;

/**
 * 事件回放缓存。
 *
 * 面板窗口可能在事件已经产生之后才加载完成（例如主窗已经握过手、连上了守护进程），
 * 缓存最近的消息并在面板窗 did-finish-load 时按序补发，面板窗就能靠同一条事件流
 * 重建出与主窗一致的终端/连接状态，渲染进程无需改动状态逻辑。
 */
const eventLog: any[] = [];
const MAX_EVENT_LOG = 2000;

function allWindows(): BrowserWindow[] {
  const list: BrowserWindow[] = [];
  if (mainWindow && !mainWindow.isDestroyed()) list.push(mainWindow);
  for (const name of PANEL_NAMES) {
    const w = panelWindows[name];
    if (w && !w.isDestroyed()) list.push(w);
  }
  return list;
}

/** 把后端事件发给所有窗口（主窗 + 队列面板 + 终端面板） */
function broadcast(msg: object): void {
  eventLog.push(msg);
  if (eventLog.length > MAX_EVENT_LOG) eventLog.splice(0, eventLog.length - MAX_EVENT_LOG);
  for (const w of allWindows()) w.webContents.send('ranarch:event', msg);
}

function ensureClient(): IpcClient {
  if (!client) {
    socketPath = resolveSocketPath();
    client = new IpcClient(
      socketPath,
      (msg) => broadcast(msg),
      // 客户端自己的 detail（连接失败原因等）优先；没有的时候补上「后端正在
      // 安装 / 正在启动」这类进度说明，否则用户在认证框那段时间里什么都看不到
      (state, detail) =>
        broadcast({
          type: 'conn',
          state,
          socketPath,
          detail: detail || (state === 'connected' ? '' : backendNote),
        }),
    );
  }
  return client;
}

// ---------- 后端生命周期 ----------
//
// 后端必须以 root 运行（要往 / 里写文件、要调 pacman），前端绝不能提权，
// 所以「前后端合并」指的是**分发形态**合成了一个文件：后端产物就躺在
// AppImage 的 resources/backend/ 里（见 scripts/stage-backend.sh），
// 用户只需要下载一个 AppImage，不必再自己 cmake --install。
//
// 启动逻辑按优先级：
//   1. 系统守护进程可用 → 什么都不做，直接连
//   2. 本会话已有会话级后端 → 什么都不做，直接连
//   3. 否则：
//      a. /usr/bin/ranarch-daemon 不存在 → 先用 pkexec 跑随包的安装脚本（一次性）
//      b. 生成会话配置，用 pkexec 拉起会话级后端
//         （--exit-with-pid 盯住前端进程、--socket-owner 把 socket 交给当前用户）
//
// pkexec 的授权靠 polkit 动作里的 exec.path 精确匹配，所以：
//   - 启动后端（/usr/bin/ranarch-daemon）命中我们自己的
//     org.ranarch.daemon.launch，认证框是品牌文案；
//   - 首次安装那一次跑的是 AppImage 挂载点里的脚本，匹配不到我们的动作，
//     走系统通用的 org.freedesktop.policykit.exec，文案是通用的。

/** 用 pkexec 跑一个程序，等它结束；退出码 126 表示用户取消了认证 */
function runPkexec(args: string[]): Promise<void> {
  return new Promise((resolve, reject) => {
    const child = spawn('pkexec', args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let stderr = '';
    child.stdout?.on('data', (d: Buffer) => process.stdout.write(d));
    child.stderr?.on('data', (d: Buffer) => {
      const text = d.toString();
      stderr += text;
      process.stderr.write(text);
    });
    child.on('error', (err) => reject(new Error(`无法执行 pkexec：${err.message}`)));
    child.on('close', (code) => {
      if (code === 0) return resolve();
      if (code === 126) return reject(new Error('认证被取消或未获授权'));
      reject(new Error(stderr.trim().split('\n').pop() || `pkexec 退出码 ${code}`));
    });
  });
}

/**
 * 一次性把随包的后端装进 /usr（守护进程 / CLI / libranarch / polkit 策略 /
 * systemd unit / 默认配置 / 运行期目录）。幂等，可以重复跑。
 */
async function installBackend(): Promise<void> {
  const script = path.join(BACKEND_DIR, 'install-backend.sh');
  if (!fs.existsSync(script)) {
    throw new Error(`随包后端不完整，缺少 ${script} —— 请先用 npm run stage:backend 打包`);
  }
  // 用 bash 读脚本，不依赖脚本自身的执行位（打包过程可能会掉权限位）
  await runPkexec(['/usr/bin/bash', script]);
}

/**
 * 生成会话级配置：以系统配置为底（这样装包路径是真实的），只把 socket 和
 * 日志挪到本会话目录，避免和系统守护进程抢文件。
 */
function writeSessionConfig(): void {
  const bundledExample = path.join(BACKEND_DIR, 'etc/ranarch.conf.example');
  const base = fs.existsSync(DEFAULT_CONFIG) ? DEFAULT_CONFIG : bundledExample;
  const text = fs
    .readFileSync(base, 'utf-8')
    .replace(/^(\s*socket_path\s*=).*$/m, `$1 ${SESSION_SOCK}`)
    .replace(/^(\s*log_file\s*=).*$/m, `$1 ${SESSION_LOG}`);
  fs.mkdirSync(path.dirname(SESSION_CONF), { recursive: true, mode: 0o700 });
  fs.writeFileSync(SESSION_CONF, text, { mode: 0o600 });
}

/**
 * 拉会话级后端。pkexec 会一直等到守护进程退出，所以这里不 await ——
 * 守护进程盯着前端 pid（--exit-with-pid），前端一退它自己就结束。
 */
function spawnSessionDaemon(): void {
  const child = spawn(
    'pkexec',
    [
      '/usr/bin/ranarch-daemon',
      '--config',
      SESSION_CONF,
      '-f',
      `--exit-with-pid=${process.pid}`,
      `--socket-owner=${MY_UID}`,
    ],
    { stdio: 'ignore', detached: true },
  );
  child.on('error', (err) => {
    backendNote = `拉起后端失败：${err.message}`;
  });
  child.unref();
}

/** 等 socket 就绪（pkexec 要等用户输密码，所以给得宽一点） */
async function waitForSocket(sock: string, timeoutMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (socketUsable(sock)) return true;
    await new Promise((r) => setTimeout(r, 300));
  }
  return socketUsable(sock);
}

let backendReady = false; // 只尝试拉起一次，避免反复弹认证框

/**
 * 确保后端在跑。失败不阻塞界面：客户端本来就会一直重连，用户接好认证后
 * 后端起来就能连上。返回 socket 路径。
 */
async function ensureBackend(): Promise<string> {
  // 开发/测试模式由外部指定，不插手
  if (process.env.RANARCH_SOCKET) return resolveSocketPath();
  if (backendReady) return resolveSocketPath();

  if (socketUsable(DEFAULT_SOCKET) || socketUsable(SESSION_SOCK)) {
    backendReady = true;
    return resolveSocketPath();
  }

  const note = (text: string) => {
    backendNote = text;
    broadcast({ type: 'conn', state: 'connecting', socketPath: SESSION_SOCK, detail: text });
  };

  try {
    if (!fs.existsSync('/usr/bin/ranarch-daemon')) {
      note('首次运行：正在安装后端（需要授权）…');
      await installBackend();
    }
    note('正在启动后端（需要授权）…');
    writeSessionConfig();
    spawnSessionDaemon();
    if (await waitForSocket(SESSION_SOCK, 30_000)) {
      backendNote = '';
      backendReady = true;
    } else {
      note('后端启动超时 —— 认证是否被取消了？');
    }
  } catch (err) {
    note(`后端启动失败：${(err as Error).message}`);
  }
  return resolveSocketPath();
}

// ---------- 窗口 ----------

/** 应用图标路径：打包后是 resources/icon.png（extraResources），开发时直接用 public 里的原件 */
const APP_ICON = app.isPackaged
  ? path.join(process.resourcesPath, 'icon.png')
  : path.join(__dirname, '../public/icon.png');

/**
 * 三个窗口共用的无边框外观。
 *
 * 硬约束：任何窗口都不改变尺寸（唯一例外是队列开合时终端的横跨宽度），
 * 所以这里 resizable / maximizable 全部关掉，尺寸由代码写死。
 */
const BASE_WINDOW_OPTIONS = {
  frame: false,
  // 宽高按「web 页面尺寸」解释；无边框窗口下窗口尺寸即内容尺寸
  useContentSize: true,
  hasShadow: false,
  resizable: false,
  skipTaskbar: true,
  show: false,
  // 取应用本体的底色：窗口尚未绘制时用它填充，不会露出黑色
  backgroundColor: '#E4DDD8',
  icon: APP_ICON,
  webPreferences: {
    preload: path.join(__dirname, 'preload.js'),
    contextIsolation: true,
    nodeIntegration: false,
    sandbox: true,
  },
};

/** 面板窗口的额外约束：卫星窗，不能最小化 / 最大化 */
const PANEL_WINDOW_OPTIONS = {
  minimizable: false,
  maximizable: false,
};

/** 三个窗口加载同一份 index.html，面板窗口用 query 表明自己承载哪块面板 */
function loadWindow(win: BrowserWindow, panel: 'main' | PanelName): void {
  if (process.env.VITE_DEV_SERVER_URL) {
    win.loadURL(`${process.env.VITE_DEV_SERVER_URL}?panel=${panel}`);
  } else {
    win.loadFile(path.join(__dirname, '../dist/index.html'), { query: { panel } });
  }
}

/**
 * 面板尺寸：设计尺寸与「主窗边缘到工作区边界」的可用空间取小。
 *
 * 为什么需要这一步：主窗 600 + 终端 410 = 1010px，而带任务栏的屏幕工作区常常不到
 * 1010px。若硬把窗口放到主窗下方，窗口管理器会把它夹回屏幕内 —— 结果就是面板压在主窗
 * 身上。所以这里主动把面板「沿生长方向」收缩到剩余空间：共享边仍然精确贴齐，装配体
 * 依旧是一个完整矩形，只会比设计尺寸矮 / 窄一点，绝不会压到主窗或跑到屏幕外。
 */
function panelSize(name: PanelName): { width: number; height: number } {
  const o = origin();
  const wa = screen.getDisplayMatching(mainWindow!.getBounds()).workArea;
  const right = Math.max(0, wa.x + wa.width - (o.x + MAIN_W)); // 主窗右侧还剩多少
  const below = Math.max(0, wa.y + wa.height - (o.y + MAIN_H)); // 主窗下方还剩多少
  const fit = (want: number, avail: number) => Math.max(PANEL_MIN, Math.min(want, avail));
  if (name === 'queue') {
    // 队列在右侧生长，高度与主窗齐平（主窗在屏幕内，这个高度必然放得下）
    return { width: fit(QUEUE_W, right), height: QUEUE_H };
  }
  // 终端的窗口宽度：默认只占主区宽（不随队列开合变化）；打开「横跨队列」后才在
  // 队列展开时变成「主区 + 队列」，把队列下方那块也填满。
  const queueW = fit(QUEUE_W, right);
  return {
    width: MAIN_W + (panelSpan && panelOpen.queue ? queueW : 0),
    height: fit(TERMINAL_H, below),
  };
}

/**
 * 把面板窗口摆到贴齐位置（零间隙）并顺便校正尺寸。
 *
 * 面板窗口的坐标全程只有这一处写入：它是位置的唯一真相，动画完全交给面板窗里的
 * CSS 内容位移。窗口自己不动，也就没有逐帧 setBounds 带来的抖动与残影。
 */
function layoutPanel(name: PanelName): void {
  const win = panelWindows[name];
  if (!win || win.isDestroyed()) return;
  const size = panelSize(name);
  const rest = restPosition(name);
  const b = win.getBounds();
  if (b.x === rest.x && b.y === rest.y && b.width === size.width && b.height === size.height) return;
  win.setBounds({ x: rest.x, y: rest.y, width: size.width, height: size.height });
}

/**
 * 装配体让位：面板要往右 / 往下长时，先把整块装配体往左 / 往上让，直到主窗贴到工作区边缘。
 *
 * 让位量 = 缺多少空间，但不超过主窗那一侧的余量。让不出来（屏幕本身就不够大）的部分
 * 才由 panelSize 收缩面板兜底 —— 所以「三块窗口共享空间」优先，「压扁面板」是最后手段，
 * 而「压住主界面」永远不会发生。
 *
 * 两个方向都要算：队列往右长（横向缺空间就整体左移），终端往下长（纵向缺空间就整体上移）。
 */
function applyAssemblyFit(): void {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  if (mainWindow.isMaximized() || mainWindow.isFullScreen()) return;
  const b = mainWindow.getBounds();
  if (naturalX === null) naturalX = b.x + assemblyShiftX;
  if (naturalY === null) naturalY = b.y + assemblyShiftY;
  const wa = screen.getDisplayMatching(b).workArea;

  // 横向：队列（打开时）需要在主窗右侧占 QUEUE_W
  const rightRoom = wa.x + wa.width - (naturalX + MAIN_W);
  const needX = Math.max(0, (panelOpen.queue ? QUEUE_W : 0) - rightRoom);
  assemblyShiftX = Math.min(needX, Math.max(0, naturalX - wa.x));

  // 纵向：终端（打开且横跨队列时，宽度也可能要更多）需要在主窗下方占 TERMINAL_H
  const bottomRoom = wa.y + wa.height - (naturalY + MAIN_H);
  const needY = Math.max(0, (panelOpen.terminal ? TERMINAL_H : 0) - bottomRoom);
  assemblyShiftY = Math.min(needY, Math.max(0, naturalY - wa.y));

  const wantX = naturalX - assemblyShiftX;
  const wantY = naturalY - assemblyShiftY;
  if (wantX !== b.x || wantY !== b.y) mainWindow.setBounds({ x: wantX, y: wantY });
}

/**
 * 三块窗口共用的原点（主窗左上角，取整）。
 *
 * 用「自然位置 - 让位偏移」而不是 getBounds()：让位是 setBounds 出去、由窗口管理器
 * 异步落地的，直接读回可能在头一两帧还是旧值，面板就会滞后一帧再跳一下。
 * 用户拖窗时 naturalX / naturalY 会被 move 事件同步刷新，所以这里始终等于主窗真实位置。
 */
function origin(): { x: number; y: number } {
  const b = mainWindow!.getBounds();
  return {
    x: Math.round(naturalX === null ? b.x : naturalX - assemblyShiftX),
    y: Math.round(naturalY === null ? b.y : naturalY - assemblyShiftY),
  };
}

/** 面板贴齐主窗的静止位置（零间隙）：队列在主窗右侧，终端在主窗下方 */
function restPosition(name: PanelName): { x: number; y: number } {
  const o = origin();
  return name === 'queue'
    ? { x: o.x + MAIN_W, y: o.y }
    : { x: o.x, y: o.y + MAIN_H };
}

function panelState(): PanelState {
  return {
    queueOpen: panelOpen.queue,
    terminalOpen: panelOpen.terminal,
    terminalSpan: panelSpan,
  };
}

/** 终端「横跨队列」开关：窗口宽度随之改变，所以要让位 / 重新摆放后再广播状态 */
function setTerminalSpan(span: boolean): void {
  if (panelSpan === span) return;
  panelSpan = span;
  applyAssemblyFit();
  for (const n of PANEL_NAMES) layoutPanel(n);
  broadcastPanelState();
}

/** 把面板开合状态广播给所有窗口（菜单按钮的按下态、面板内容的滑出 / 收入都据此同步） */
function broadcastPanelState(): void {
  const state = panelState();
  for (const w of allWindows()) w.webContents.send('ranarch:panelState', state);
}

function cancelHide(name: PanelName): void {
  if (panelHideTimers[name]) {
    clearTimeout(panelHideTimers[name]!);
    panelHideTimers[name] = null;
  }
}

/**
 * 收入动画播完后再隐藏窗口。
 *
 * 这一步是「残影闪烁」的解药：面板窗口透明、位置固定，收入结束时内容已经被剪裁回
 * 主窗身位之外（窗外），此时窗口上本来就什么都没有，再隐藏它不可能留下任何残像；
 * 反过来若在动画刚开始就隐藏，合成器还会把上一帧的面板画面留在屏幕上。
 */
function scheduleHide(name: PanelName): void {
  cancelHide(name);
  panelHideTimers[name] = setTimeout(() => {
    panelHideTimers[name] = null;
    if (panelOpen[name]) return; // 期间又被打开
    const win = panelWindows[name];
    if (win && !win.isDestroyed() && win.isVisible()) win.hide();
  }, PANEL_CLOSE_MS + PANEL_HIDE_GRACE_MS);
}

/**
 * 开合面板：记录状态 → 让位 → 摆好窗口 → 显示 / 安排隐藏 → 广播新状态。
 *
 * 窗口本身不参与动画：它只被摆到贴齐位置，内容的滑出 / 收入由面板窗里的 CSS 完成
 * （见 body[data-panel] 那组规则）。这样面板永远只画在「自己那一块」里，重叠在主窗
 * 上的那一段被窗口裁掉 —— 既不会叠在主界面上面，也不会有逐帧移动窗口的抖动。
 */
function setPanelOpen(name: PanelName, open: boolean): void {
  if (panelOpen[name] === open) return;
  panelOpen[name] = open;
  applyAssemblyFit();
  for (const n of PANEL_NAMES) layoutPanel(n);

  const win = panelWindows[name];
  if (win && !win.isDestroyed()) {
    if (open) {
      cancelHide(name);
      // 先显形（此刻内容还在窗外，看不到任何东西），再广播状态让内容滑出来
      if (!win.isVisible()) win.showInactive();
    } else {
      scheduleHide(name);
    }
  }
  broadcastPanelState();
}

/** 主窗移动 / 让位后，把面板重新摆到贴齐位置（位置是唯一真相，动画不参与） */
function syncPanelPositions(): void {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  if (mainWindow.isMaximized() || mainWindow.isFullScreen()) return;
  for (const name of PANEL_NAMES) layoutPanel(name);
}

/** 主窗最大化 / 全屏时把面板藏起来（此时没有可用的落点），逻辑开合状态保持不变 */
function hideAllPanels(): void {
  for (const name of PANEL_NAMES) {
    cancelHide(name);
    const win = panelWindows[name];
    if (win && !win.isDestroyed() && win.isVisible()) win.hide();
  }
}

/** 主窗从最大化 / 全屏还原后，按逻辑状态把面板重新摆出来（内容本来就停在展开态） */
function restorePanels(): void {
  for (const name of PANEL_NAMES) {
    if (!panelOpen[name]) continue;
    const win = panelWindows[name];
    if (!win || win.isDestroyed()) continue;
    layoutPanel(name);
    win.showInactive();
  }
}

function createPanelWindow(name: PanelName): void {
  if (!mainWindow) return;
  const size = panelSize(name);
  const rest = restPosition(name);
  const win = new BrowserWindow({
    ...BASE_WINDOW_OPTIONS,
    ...PANEL_WINDOW_OPTIONS,
    // 面板窗透明：窗口只是「一块画布」，内容滑出前窗外什么都没有，
    // 重叠在主窗上的那一段被窗口裁掉 —— 面板永远压不到主界面上。
    // 注意必须显式给全透明的 backgroundColor：公共配置里那个不透明底色会把
    // 窗口的空白区域填成实心色（透明窗口上这个值不会自动变透明）。
    transparent: true,
    backgroundColor: '#00000000',
    width: size.width,
    height: size.height,
    x: rest.x,
    y: rest.y,
    // 从属关系：面板随主窗一起最小化 / 关闭
    parent: mainWindow,
    title: name === 'queue' ? 'RanArch Queue' : 'RanArch Terminal',
  });
  panelWindows[name] = win;
  loadWindow(win, name);

  win.webContents.on('did-finish-load', () => {
    // 先按序补发事件流重建后端相关状态，再补发队列快照与面板开合状态
    for (const msg of eventLog) win.webContents.send('ranarch:event', msg);
    if (lastSnapshot) win.webContents.send('ranarch:sync', lastSnapshot);
    win.webContents.send('ranarch:panelState', panelState());
  });

  win.on('closed', () => { panelWindows[name] = null; });
}

function createWindow(): void {
  mainWindow = new BrowserWindow({
    ...BASE_WINDOW_OPTIONS,
    // 内容尺寸恒定 600×600 —— 这是整套架构的硬约束，任何时候都不改
    width: MAIN_W,
    height: MAIN_H,
    title: 'RanArch Installer',
  });

  loadWindow(mainWindow, 'main');
  mainWindow.once('ready-to-show', () => mainWindow?.show());
  // 加载失败时也要把窗口露出来，否则用户只会看到「什么都没发生」
  mainWindow.webContents.on('did-fail-load', () => mainWindow?.show());

  // 主窗移动：用户拖窗后把「自然位置」更新到新位置（让位偏移要反推掉），
  // 再把可见面板带到固定相对位置。自己让位引起的 move 在这里是幂等的。
  mainWindow.on('move', () => {
    const b = mainWindow?.getBounds();
    if (b) {
      naturalX = b.x + assemblyShiftX;
      naturalY = b.y + assemblyShiftY;
    }
    syncPanelPositions();
  });
  mainWindow.on('maximize', hideAllPanels);
  mainWindow.on('enter-full-screen', hideAllPanels);
  mainWindow.on('unmaximize', restorePanels);
  mainWindow.on('leave-full-screen', restorePanels);

  mainWindow.on('closed', () => {
    // 子窗口有 parent 关系，会随主窗一起销毁；这里只清引用与定时器，
    // 避免后续 IPC 访问到已销毁的对象
    mainWindow = null;
    for (const name of PANEL_NAMES) {
      cancelHide(name);
      const w = panelWindows[name];
      panelWindows[name] = null;
      if (w && !w.isDestroyed()) w.destroy();
    }
  });

  createPanelWindow('queue');
  createPanelWindow('terminal');
}

// ---------- IPC handlers ----------

ipcMain.handle('ranarch:status', () => {
  return ensureClient().getState();
});

ipcMain.handle('ranarch:pickFiles', async () => {
  const result = await dialog.showOpenDialog(mainWindow!, {
    title: '选择安装包',
    properties: ['openFile', 'multiSelections'],
    filters: [{ name: '安装包', extensions: ['deb', 'rpm'] }],
  });
  // 返回结构化元数据：真实文件大小 + 从文件名解析的版本/格式
  return result.filePaths.map((p) => {
    const name = path.basename(p);
    let size = '—';
    try {
      size = formatSize(fs.statSync(p).size);
    } catch { /* 读取失败则留空 */ }
    const m = name.match(/[_-](\d+[0-9A-Za-z.+~-]*)[_-]/);
    return {
      path: p,
      name,
      format: name.toLowerCase().endsWith('.rpm') ? 'rpm' : 'deb',
      version: m ? m[1] : '—',
      size,
    };
  });
});

ipcMain.handle('ranarch:install', async (_ev, filePath: string, options: any) => {
  const c = ensureClient();
  if (c.getState().state !== 'connected') {
    throw new Error(`未连接到守护进程 (${c.getState().socketPath})`);
  }
  // 后端先回 {"type":"ok","session_id":...}，随后以 step/done 事件流推送
  const ack = await c.request({ type: 'install', path: filePath, options }, ['ok'], 15000);
  return ack.session_id as string;
});

ipcMain.handle('ranarch:remove', async (_ev, packageId: number) => {
  const c = ensureClient();
  await c.request({ type: 'remove', package_id: packageId }, ['done'], 60000);
  return true;
});

ipcMain.handle('ranarch:list', async () => {
  const c = ensureClient();
  if (c.getState().state !== 'connected') return [];
  const resp = await c.request({ type: 'list' }, ['packages'], 10000);
  return resp.packages ?? [];
});

/**
 * 读取守护进程策略（[checks] / [signature] / [sandbox] / [deps]）。
 * 每次都重新读盘，这样用户改了 ranarch.conf 后重开设置面板就能看到新值。
 */
ipcMain.handle('ranarch:daemonPolicy', () => readDaemonPolicy());

/**
 * 回答依赖决策提示。
 * 守护进程发完 prompt 后会在同一连接上阻塞读取，最长 5 分钟，
 * 因此这里用 send（不等回执）而不是 request。
 */
ipcMain.handle('ranarch:answer', (_ev, sessionId: string, promptId: number,
                                  choice: string, archName: string) => {
  const c = ensureClient();
  if (c.getState().state !== 'connected') {
    throw new Error('未连接到守护进程，无法回答依赖提示');
  }
  c.send({
    type: 'answer',
    session_id: sessionId,
    prompt_id: promptId,
    choice,
    ...(archName ? { arch_name: archName } : {}),
  });
  return true;
});

/** 查询会话状态与事件回放（用于失败详情页展示后端记录） */
ipcMain.handle('ranarch:info', async (_ev, sessionId: string) => {
  const c = ensureClient();
  if (c.getState().state !== 'connected') {
    throw new Error('未连接到守护进程');
  }
  return c.request({ type: 'info', session_id: sessionId }, ['session', 'error'], 10000);
});

ipcMain.on('ranarch:minimize', () => mainWindow?.minimize());
ipcMain.on('ranarch:maximize', () => {
  if (!mainWindow) return;
  if (mainWindow.isMaximized()) mainWindow.unmaximize();
  else mainWindow.maximize();
});
ipcMain.on('ranarch:close', () => mainWindow?.close());

/**
 * 面板开合请求。
 *
 * 渲染进程只说「我要队列 / 终端开着还是关着」，主进程是唯一权威：
 * 记录状态、驱动窗口抽出 / 收入动画，再把新状态广播给所有窗口，
 * 这样三个窗口上的菜单按钮 / 收起按钮的按下态永远一致。
 * 不传 open 时按当前状态取反；面板里的「收起」按钮与自动展开传显式状态。
 */
ipcMain.on('ranarch:toggleQueue', (_ev, open?: boolean) => {
  setPanelOpen('queue', typeof open === 'boolean' ? open : !panelOpen.queue);
});

ipcMain.on('ranarch:toggleTerminal', (_ev, open?: boolean) => {
  setPanelOpen('terminal', typeof open === 'boolean' ? open : !panelOpen.terminal);
});

/** 终端「横跨队列」开关（选项菜单，前端把偏好持久化后再送过来） */
ipcMain.on('ranarch:setTerminalSpan', (_ev, span: unknown) => {
  setTerminalSpan(span === true);
});

/** 状态栏用：真实 CPU / 内存占用（读 /proc，主进程才有文件系统权限） */
ipcMain.handle('ranarch:sysUsage', () => readSystemUsage());

/**
 * 清空终端日志。
 *
 * 日志 DOM 在每个窗口里各有一份（主窗那份是隐藏的，终端窗那份在显示），
 * 所以由主进程统一广播，三个窗口各自清掉自己那份，界面才会一致。
 */
ipcMain.on('ranarch:clearLog', () => {
  for (const w of allWindows()) w.webContents.send('ranarch:clearLog');
});

/**
 * 跨窗口状态同步。
 *
 * 队列内容与安装进度是渲染进程的本地状态（不在后端事件流里），因此由主窗把它们
 * 序列化成快照广播给面板窗；反过来，面板窗上的操作（一键安装 / 移除 / 排序）
 * 通过 ranarch:command 回传给主窗执行 —— 安装流程只在主窗里跑一份。
 */
ipcMain.on('ranarch:sync', (ev, snapshot: unknown) => {
  lastSnapshot = snapshot;
  for (const w of allWindows()) {
    if (w.webContents.id === ev.sender.id) continue;
    w.webContents.send('ranarch:sync', snapshot);
  }
});

ipcMain.on('ranarch:command', (ev, cmd: unknown) => {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  if (mainWindow.webContents.id === ev.sender.id) return; // 主窗自己直接执行
  mainWindow.webContents.send('ranarch:command', cmd);
});

// ---------- 生命周期 ----------

app.whenReady().then(() => {
  createWindow();
  // 先连上（连不上会自己重试），同时把后端拉起来 —— 两条线并行，
  // 后端就绪后重连自然成功
  ensureClient().start();
  void ensureBackend();
});

app.on('window-all-closed', () => {
  client?.stop();
  client = null;
  if (process.platform !== 'darwin') app.quit();
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
});
