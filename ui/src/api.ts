// RanArch Installer — 渲染进程 API 层
//
// 通过 Electron preload 暴露的 window.ranarch 与主进程通信。
// 浏览器环境 fallback 到 Mock 实现（发出与真实守护进程同构的事件），
// 以便在不启动 daemon 的情况下开发调试。
//
// 协议依据：docs/frontend-integration.md

import type { CheckToggles, DaemonPolicy } from './settings';

export type { DaemonPolicy };

// ---------- 与后端协议一致的类型 ----------

/** 依赖决策提示（对应后端 build_dependency_prompt） */
export interface DependencyPrompt {
  session_id: string;
  prompt_id: number;
  kind: 'dependency';
  dependency: {
    raw_name: string;
    arch_candidate: string;
    op: string;
    version: string;
    /** false 表示映射表里没有对应 Arch 包，用户必须自己输入包名 */
    mapped: boolean;
  };
  options: Array<'install' | 'skip' | 'map' | 'abort'>;
}

/** 依赖决策的选择 */
export type DepChoice = 'install' | 'skip' | 'map' | 'abort';

/** 会话回放（对应 info 请求的 session 响应） */
export interface SessionInfo {
  session_id: string;
  file_path: string;
  active: boolean;
  done: boolean;
  /** 事件日志，每个元素本身是一段 JSON 文本，需要二次解析 */
  events: string[];
  ok?: boolean;
  summary?: string;
  installed_files_count?: number;
}

/** 与后端 ipc_server.cpp 推送的消息类型对齐 */
export interface InstallEvent {
  type:
    | 'step' | 'done' | 'ok' | 'error' | 'packages'
    | 'checks_disabled' | 'prompt' | 'pty_start' | 'pty_data' | 'pty_end'
    | 'session'        // info 响应
    | 'conn';          // 仅前端内部：连接状态变化

  // --- step ---
  step?: string;
  detail?: string;

  // --- ok / prompt / answer（会话标识） ---
  session_id?: string;

  // --- done（顶层字段，非嵌套 result） ---
  ok?: boolean;
  summary?: string;
  installed_files_count?: number;
  error_code?: string;
  error_message?: string;

  // --- checks_disabled ---
  checks?: string[];

  // --- prompt ---
  prompt_id?: number;
  kind?: string;
  dependency?: DependencyPrompt['dependency'];
  options?: string[];

  // --- pty_* ---
  cmd?: string;
  /** base64 编码的终端字节流，不能当普通字符串处理 */
  data?: string;
  exit_code?: number;

  // --- error（协议级错误，code 为 SCREAMING_SNAKE） ---
  code?: string;
  message?: string;

  // --- packages（list 响应） ---
  packages?: DaemonPackage[];

  // --- session（info 响应） ---
  file_path?: string;
  active?: boolean;
  done?: boolean;
  events?: string[];

  // --- conn（前端内部） ---
  state?: ConnState;
  socketPath?: string;
}

/** 守护进程 DB 中的软件包记录（list 响应元素） */
export interface DaemonPackage {
  id: number;
  name: string;
  version: string;
  source_format: string;
  original_file: string;
  signature_status: string;
  install_date: number;
}

export type ConnState = 'connecting' | 'connected' | 'disconnected';

export interface ConnectionInfo {
  state: ConnState;
  socketPath: string;
  error?: string;
}

/** 安装结果（由 done 事件顶层字段归一化得到） */
export interface InstallOutcome {
  ok: boolean;
  summary: string;
  installed_files_count: number;
  error_code: string;
  error_message: string;
}

// ---------- 队列条目 ----------

export interface PackageInfo {
  id: number;
  name: string;
  version: string;
  format: 'deb' | 'rpm';
  size: string;
  path: string;
  status: 'queued' | 'installing' | 'done' | 'error';
  sig: 'signed_ok' | 'unsigned' | 'bad';
  /** 安装会话 id（收到 ok 事件后填入），用于失败时回放事件日志 */
  session_id?: string;
  error_code?: string;
  error_message?: string;
}

/** 用户选中的待安装文件（由主进程 stat + 文件名解析得到） */
export interface PickedFile {
  path: string;
  name: string;
  format: 'deb' | 'rpm';
  version: string;
  size: string;
}

/** install 请求的 options（与后端 InstallOptions 对齐） */
export interface InstallOptions {
  interactive: boolean;
  dry_run: boolean;
  checks: CheckToggles;
}

/** 面板开合状态（由主进程广播，所有窗口共享同一份） */
export interface PanelState {
  queueOpen: boolean;
  terminalOpen: boolean;
  /** 终端是否横跨到队列下方（选项菜单里的开关） */
  terminalSpan: boolean;
}

/** 系统占用（状态栏 CPU / 内存，主进程读 /proc 采样） */
export interface SystemUsage {
  /** CPU 使用率 0–100 */
  cpu: number;
  mem: {
    /** 内存占用率 0–100 */
    percent: number;
    usedMb: number;
    totalMb: number;
  };
}

/** 阶段记录（主区与终端步骤条共用） */
export interface SyncStep {
  step: string;
  detail?: string;
}

/**
 * 跨窗口状态同步快照。
 *
 * 队列内容与安装进度是渲染进程的本地状态（不在后端事件流里），
 * 由主窗序列化后广播给面板窗口，面板窗口据此重建出同一份队列。
 */
export interface SyncSnapshot {
  queue: PackageInfo[];
  done: PackageInfo[];
  installing: boolean;
  sessionTotal: number;
  sessionDone: number;
  currentStep: string;
  currentPackage: PackageInfo | null;
  steps: SyncStep[];
}

/** 面板窗回传给主窗的操作命令（安装流程只在主窗里执行） */
export type PanelCommand =
  | { cmd: 'installAll' }
  | { cmd: 'remove'; id: number }
  | { cmd: 'reorder'; fromId: number; toId: number; before: boolean }
  | { cmd: 'retry'; id: number };

export interface Backend {
  status(): Promise<ConnectionInfo>;
  daemonPolicy(): Promise<DaemonPolicy>;
  pickFiles(): Promise<PickedFile[]>;
  install(path: string, options: InstallOptions): Promise<string>;
  /** 回答依赖决策提示；必须在收到 prompt 后立即于同一连接发出 */
  answer(sessionId: string, promptId: number, choice: DepChoice, archName?: string): Promise<void>;
  /** 查询会话状态与事件回放 */
  info(sessionId: string): Promise<SessionInfo>;
  remove(packageId: number): Promise<void>;
  list(): Promise<DaemonPackage[]>;
  onEvent(cb: (ev: InstallEvent) => void): () => void;
  minimize(): void;
  maximize(): void;
  close(): void;
  /** 开合面板窗口（主进程记录状态、驱动窗口抽出 / 收入动画并广播新状态） */
  toggleQueue(open?: boolean): void;
  toggleTerminal(open?: boolean): void;
  /** 订阅面板开合状态（主进程是唯一权威，三个窗口据此同步按钮按下态） */
  onPanelState(cb: (state: PanelState) => void): () => void;
  /** 设置终端是否横跨到队列下方（由主进程统一改窗口宽度） */
  setTerminalSpan(span: boolean): void;
  /** 状态栏用：真实 CPU / 内存占用 */
  sysUsage(): Promise<SystemUsage>;
  /** 清空终端日志（主进程广播给所有窗口） */
  clearLog(): void;
  onClearLog(cb: () => void): () => void;
  /** 主窗广播队列/进度快照 */
  syncState(snapshot: SyncSnapshot): void;
  /** 面板窗接收主窗快照 */
  onSyncState(cb: (snapshot: SyncSnapshot) => void): () => void;
  /** 面板窗把用户操作回传主窗 */
  sendCommand(cmd: PanelCommand): void;
  /** 主窗接收面板窗回传的操作 */
  onCommand(cb: (cmd: PanelCommand) => void): () => void;
}

declare global {
  interface Window {
    ranarch?: Backend;
  }
}

// ---------- 共享工具 ----------

/** 从文件名解析出版本号，如 nginx_1.24.0_x86_64.rpm → 1.24.0 */
export function parseVersionFromName(name: string): string {
  const m = name.match(/[_-](\d+[0-9A-Za-z.+~-]*)[_-]/);
  return m ? m[1] : '—';
}

export function parseFormatFromName(name: string): 'deb' | 'rpm' {
  return name.toLowerCase().endsWith('.rpm') ? 'rpm' : 'deb';
}

export function formatSize(bytes: number): string {
  if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + ' MB';
  if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return bytes + ' B';
}

/** 把 done 事件的顶层字段归一化为安装结果 */
export function toOutcome(ev: InstallEvent): InstallOutcome {
  return {
    ok: ev.ok === true,
    summary: ev.summary ?? '',
    installed_files_count: ev.installed_files_count ?? 0,
    error_code: ev.error_code ?? '',
    error_message: ev.error_message ?? '',
  };
}

/**
 * 解码 pty_data 的 base64 负载。
 * 终端输出含 ANSI 转义与半截 UTF-8，因此按字节解码并保留流式状态。
 */
export function makePtyDecoder(): (b64: string) => string {
  const decoder = new TextDecoder('utf-8');
  return (b64: string) => {
    try {
      const bin = atob(b64);
      const bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      return decoder.decode(bytes, { stream: true });
    } catch {
      return '';
    }
  };
}

/** 去掉 ANSI 转义序列，便于在普通 DOM 中显示终端输出 */
export function stripAnsi(s: string): string {
  return s
    .replace(/\x1b\[[0-9;?]*[ -/]*[@-~]/g, '')
    .replace(/\x1b\][^\x07]*\x07/g, '')
    .replace(/\x1b[@-Z\\-_]/g, '')
    .replace(/\r/g, '');
}

// ---------- Mock 实现（浏览器开发用） ----------

const MOCK_FILES: PickedFile[] = [
  { path: '/tmp/hello-world_1.0.0-1_amd64.deb', name: 'hello-world_1.0.0-1_amd64.deb', format: 'deb', version: '1.0.0-1', size: '12.4 KB' },
  { path: '/tmp/libssl-dev_3.0.2_amd64.deb', name: 'libssl-dev_3.0.2_amd64.deb', format: 'deb', version: '3.0.2', size: '2.1 MB' },
  { path: '/tmp/nginx_1.24.0_x86_64.rpm', name: 'nginx_1.24.0_x86_64.rpm', format: 'rpm', version: '1.24.0', size: '1.2 MB' },
  { path: '/tmp/postgresql-15_15.3_amd64.deb', name: 'postgresql-15_15.3_amd64.deb', format: 'deb', version: '15.3', size: '18.5 MB' },
];

const MOCK_ERRORS = ['DEP_UNRESOLVED', 'CONFLICT_PACMAN', 'SIGNATURE_BAD', 'EXTRACT_DISK_FULL'];

class MockBackend implements Backend {
  private listeners: ((ev: InstallEvent) => void)[] = [];

  async status(): Promise<ConnectionInfo> {
    return { state: 'connected', socketPath: '/run/ranarch/ranarch.sock (mock)' };
  }

  /** Mock 的守护进程策略：与真实配置结构一致，[checks] 沙箱试装默认关闭 */
  async daemonPolicy(): Promise<DaemonPolicy> {
    return {
      loaded: false,
      configPath: '(mock) /etc/ranarch/ranarch.conf',
      checks: { signature: true, dependencies: true, conflicts: true, path_traversal: true, sandbox_trial: false },
      signature_policy: 'warn',
      sandbox_trial_install: false,
      sandbox_backend: 'none',
      dep_default_action: 'skip',
    };
  }

  async pickFiles(): Promise<PickedFile[]> {
    const shuffled = [...MOCK_FILES].sort(() => Math.random() - 0.5);
    return shuffled.slice(0, 1 + Math.floor(Math.random() * 2));
  }

  async install(path: string, options: InstallOptions): Promise<string> {
    const sessionId = 'mock-' + Date.now().toString(36);
    this.emit({ type: 'ok', session_id: sessionId });

    // 与后端一致：只有被关闭的校验项才通过 checks_disabled 播报
    const disabled: string[] = [];
    if (!options.checks.signature) disabled.push('signature');
    if (!options.checks.dependencies) disabled.push('dependencies');
    if (!options.checks.conflicts) disabled.push('conflicts');
    if (!options.checks.path_traversal) disabled.push('path_traversal');
    if (disabled.length > 0) {
      this.emit({ type: 'checks_disabled', checks: disabled });
    }

    // 只走本次真正启用的阶段 —— 沙箱关闭时不会出现 sandbox_trial
    const steps: string[] = ['parsing'];
    if (options.checks.signature) steps.push('verifying_sig');
    if (options.checks.dependencies) steps.push('resolving_deps');
    if (options.checks.conflicts) steps.push('conflict_check');
    if (options.dry_run || options.checks.sandbox_trial) steps.push('sandbox_trial');
    steps.push('staging', 'installing', 'recording');

    const shouldFail = Math.random() < 0.35;
    const failAt = shouldFail ? steps[1 + Math.floor(Math.random() * Math.max(1, steps.length - 2))] : null;
    const errorCode = failAt ? MOCK_ERRORS[Math.floor(Math.random() * MOCK_ERRORS.length)] : '';

    let i = 0;
    const timer = setInterval(() => {
      if (i >= steps.length) {
        clearInterval(timer);
        const files = 12 + Math.floor(Math.random() * 80);
        this.emit({ type: 'step', step: 'done', detail: 'ok' });
        this.emit({
          type: 'done',
          ok: true,
          summary: `installed ${path.split('/').pop()} (${files} files)`,
          installed_files_count: files,
        });
        return;
      }
      const step = steps[i];
      // installing 的 detail 是文件数（与后端一致）
      const detail = step === 'installing'
        ? `${12 + Math.floor(Math.random() * 80)} files`
        : step === 'parsing' ? path : undefined;
      this.emit({ type: 'step', step, detail });

      if (failAt === step) {
        clearInterval(timer);
        this.emit({ type: 'step', step: 'done', detail: `error: ${step}` });
        this.emit({
          type: 'done',
          ok: false,
          summary: `installing ${path}`,
          installed_files_count: 0,
          error_code: errorCode,
          error_message: `模拟失败：${step} 阶段检测到问题`,
        });
        return;
      }
      i++;
    }, 380 + Math.random() * 260);

    return sessionId;
  }

  async answer(_sessionId: string, _promptId: number, _choice: DepChoice, _archName?: string): Promise<void> {
    // mock: 不产生真实 prompt，无需处理
  }

  async info(_sessionId: string): Promise<SessionInfo> {
    throw new Error('Mock 环境不提供会话回放');
  }

  async remove(_packageId: number): Promise<void> {
    // mock: 无实际操作
  }

  async list(): Promise<DaemonPackage[]> {
    return [];
  }

  onEvent(cb: (ev: InstallEvent) => void): () => void {
    this.listeners.push(cb);
    return () => {
      this.listeners = this.listeners.filter(l => l !== cb);
    };
  }

  minimize(): void { console.log('[mock] minimize'); }
  maximize(): void { console.log('[mock] maximize'); }
  /** 浏览器环境无法真正关窗，给出明确反馈 */
  close(): void {
    console.log('[mock] close — 浏览器环境无法关闭窗口，请在 Electron 中运行');
    window.dispatchEvent(new CustomEvent('ranarch:mock-close'));
  }
  /** 浏览器里只有一个窗口，没有可开合的面板窗口 */
  toggleQueue(open?: boolean): void {
    console.log(`[mock] toggleQueue ${open === undefined ? 'toggle' : open}`);
  }
  toggleTerminal(open?: boolean): void {
    console.log(`[mock] toggleTerminal ${open === undefined ? 'toggle' : open}`);
  }
  onPanelState(_cb: (state: PanelState) => void): () => void {
    return () => { /* mock: 无面板窗口 */ };
  }
  setTerminalSpan(_span: boolean): void {
    console.log('[mock] setTerminalSpan', _span);
  }
  /** 浏览器里读不到 /proc，用随机数模拟，保证预览时状态栏是活的 */
  async sysUsage(): Promise<SystemUsage> {
    return {
      cpu: 8 + Math.floor(Math.random() * 40),
      mem: { percent: 38 + Math.floor(Math.random() * 22), usedMb: 6300, totalMb: 16000 },
    };
  }
  clearLog(): void {
    window.dispatchEvent(new CustomEvent('ranarch:mock-clear-log'));
  }
  onClearLog(cb: () => void): () => void {
    const handler = () => cb();
    window.addEventListener('ranarch:mock-clear-log', handler);
    return () => window.removeEventListener('ranarch:mock-clear-log', handler);
  }
  syncState(_snapshot: SyncSnapshot): void {
    console.log('[mock] syncState', JSON.stringify(_snapshot));
  }
  onSyncState(_cb: (snapshot: SyncSnapshot) => void): () => void {
    return () => { /* mock: 单窗口无需同步 */ };
  }
  sendCommand(cmd: PanelCommand): void {
    console.log('[mock] sendCommand', JSON.stringify(cmd));
  }
  onCommand(_cb: (cmd: PanelCommand) => void): () => void {
    return () => { /* mock: 单窗口无需同步 */ };
  }

  private emit(ev: InstallEvent): void {
    this.listeners.forEach(cb => cb(ev));
  }
}

// ---------- 导出 ----------

export function getBackend(): Backend {
  if (window.ranarch) return window.ranarch;
  console.warn('[RanArch] 未检测到 Electron 环境，使用 Mock 后端');
  return new MockBackend();
}
