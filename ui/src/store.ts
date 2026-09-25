// RanArch Installer — 全局状态 Store (EventTarget 实现)
import type { PackageInfo, InstallEvent, ConnectionInfo } from './api';

export interface AppState {
  queue: PackageInfo[];
  done: PackageInfo[];
  terminalOpen: boolean;
  queueOpen: boolean;
  settingsOpen: boolean;
  currentSession: string | null;
  currentStep: string;
  currentPackage: PackageInfo | null;
  installing: boolean;
  /** 本轮安装的总包数 / 已完成数（用于主界面进度） */
  sessionTotal: number;
  sessionDone: number;
  /** 与 ranarch-daemon 的连接状态 */
  connection: ConnectionInfo;
}

class Store extends EventTarget {
  private state: AppState = {
    queue: [],
    done: [],
    terminalOpen: false,
    queueOpen: false,
    settingsOpen: false,
    currentSession: null,
    currentStep: 'idle',
    currentPackage: null,
    installing: false,
    sessionTotal: 0,
    sessionDone: 0,
    connection: { state: 'connecting', socketPath: '' },
  };

  get(): Readonly<AppState> {
    return this.state;
  }

  /** 触发 change 事件（会引发重渲染） */
  set(partial: Partial<AppState>): void {
    this.state = { ...this.state, ...partial };
    this.emit('change');
  }

  /** 静默更新（不触发重渲染，用于高频 step 更新） */
  setSilent(partial: Partial<AppState>): void {
    this.state = { ...this.state, ...partial };
  }

  // ---- 队列操作 ----

  addToQueue(pkg: Omit<PackageInfo, 'id'>): number {
    const id = Date.now() + Math.floor(Math.random() * 1000);
    const item: PackageInfo = { ...pkg, id };
    this.set({ queue: [...this.state.queue, item] });
    return id;
  }

  removeFromQueue(id: number): void {
    this.set({ queue: this.state.queue.filter(p => p.id !== id) });
  }

  updatePackage(id: number, partial: Partial<PackageInfo>): void {
    this.set({
      queue: this.state.queue.map(p => p.id === id ? { ...p, ...partial } : p),
    });
  }

  /** 静默更新包状态 */
  updatePackageSilent(id: number, partial: Partial<PackageInfo>): void {
    this.setSilent({
      queue: this.state.queue.map(p => p.id === id ? { ...p, ...partial } : p),
    });
  }

  moveToDone(id: number): void {
    const pkg = this.state.queue.find(p => p.id === id);
    if (!pkg) return;
    this.set({
      queue: this.state.queue.filter(p => p.id !== id),
      done: [...this.state.done, { ...pkg, status: 'done' as const }],
    });
  }

  reorderQueue(fromId: number, toId: number, before: boolean): void {
    const fromIdx = this.state.queue.findIndex(p => p.id === fromId);
    const toIdx = this.state.queue.findIndex(p => p.id === toId);
    if (fromIdx === -1 || toIdx === -1) return;
    const queue = [...this.state.queue];
    const [moved] = queue.splice(fromIdx, 1);
    queue.splice(before ? toIdx : toIdx + 1, 0, moved);
    this.set({ queue });
  }

  // ---- 安装流程 ----

  /** 开始一轮安装会话 */
  beginSession(total: number): void {
    this.set({
      installing: true,
      sessionTotal: total,
      sessionDone: 0,
      currentStep: 'parsing',
    });
  }

  startPackage(packageId: number, sessionId: string): void {
    const pkg = this.state.queue.find(p => p.id === packageId);
    this.set({
      currentSession: sessionId,
      currentPackage: pkg ?? null,
      currentStep: 'parsing',
    });
  }

  /** 处理来自后端的原始事件（不触发重渲染，由调用方控制） */
  handleEvent(ev: InstallEvent): void {
    if (ev.type === 'step' && ev.step) {
      this.setSilent({ currentStep: ev.step });
      this.emit('step', ev);
    } else if (ev.type === 'done') {
      this.emit('done', ev);
    } else if (ev.type === 'error') {
      this.emit('error', ev);
    }
  }

  /** 更新连接状态 */
  setConnection(info: ConnectionInfo): void {
    const prev = this.state.connection;
    if (prev.state === info.state && prev.socketPath === info.socketPath && prev.error === info.error) {
      return;
    }
    this.state = { ...this.state, connection: info };
    this.emit('conn', info);
  }

  /** 结算当前包 */
  finishPackage(packageId: number, ok: boolean, errorCode?: string, errorMessage?: string): void {
    if (ok) {
      const pkg = this.state.queue.find(p => p.id === packageId);
      this.state = {
        ...this.state,
        queue: this.state.queue.filter(p => p.id !== packageId),
        done: pkg ? [...this.state.done, { ...pkg, status: 'done' as const }] : this.state.done,
        sessionDone: this.state.sessionDone + 1,
        currentPackage: null,
        currentSession: null,
        currentStep: 'done',
      };
    } else {
      this.state = {
        ...this.state,
        queue: this.state.queue.map(p =>
          p.id === packageId
            ? { ...p, status: 'error' as const, error_code: errorCode, error_message: errorMessage }
            : p),
        currentPackage: null,
        currentSession: null,
      };
    }
    this.emit('change');
  }

  /** 结束整轮安装 */
  endSession(): void {
    this.set({ installing: false, currentStep: 'done' });
  }

  private emit(type: string, data?: any): void {
    this.dispatchEvent(new CustomEvent(type, { detail: data }));
  }
}

export const store = new Store();
