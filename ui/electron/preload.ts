// RanArch Installer — Electron preload (contextBridge)
import { contextBridge, ipcRenderer } from 'electron';

contextBridge.exposeInMainWorld('ranarch', {
  // 查询当前与守护进程的连接状态
  status: () => ipcRenderer.invoke('ranarch:status'),

  // 读取守护进程配置策略（[checks] / [signature] / [sandbox] / [deps]）
  daemonPolicy: () => ipcRenderer.invoke('ranarch:daemonPolicy'),

  // 打开文件选择对话框，返回结构化元数据
  pickFiles: () => ipcRenderer.invoke('ranarch:pickFiles'),

  // 发起安装，返回 session_id（后端随后以事件流推送 step/done 等）
  install: (filePath: string, options: unknown) =>
    ipcRenderer.invoke('ranarch:install', filePath, options),

  // 回答依赖决策提示（守护进程正在该连接上阻塞等待）
  answer: (sessionId: string, promptId: number, choice: string, archName = '') =>
    ipcRenderer.invoke('ranarch:answer', sessionId, promptId, choice, archName),

  // 查询会话状态与事件回放
  info: (sessionId: string) => ipcRenderer.invoke('ranarch:info', sessionId),

  // 移除已安装的包
  remove: (packageId: number) => ipcRenderer.invoke('ranarch:remove', packageId),

  // 列出守护进程数据库中已记录的软件包
  list: () => ipcRenderer.invoke('ranarch:list'),

  // 订阅后端事件流（step / done / checks_disabled / prompt / pty_* / conn ...）
  onEvent: (cb: (ev: unknown) => void) => {
    const handler = (_e: unknown, data: unknown) => cb(data);
    ipcRenderer.on('ranarch:event', handler);
    return () => ipcRenderer.removeListener('ranarch:event', handler);
  },

  // 窗口控制
  minimize: () => ipcRenderer.send('ranarch:minimize'),
  maximize: () => ipcRenderer.send('ranarch:maximize'),
  close: () => ipcRenderer.send('ranarch:close'),

  // 面板开合：队列 / 终端是独立窗口，由主进程驱动窗口位移 + 透明度动画并广播状态。
  // 省略 open 表示「切换到相反状态」；传布尔值表示「切到这个状态」（收起按钮 / 自动展开用）。
  toggleQueue: (open?: boolean) => ipcRenderer.send('ranarch:toggleQueue', open),
  toggleTerminal: (open?: boolean) => ipcRenderer.send('ranarch:toggleTerminal', open),

  // 面板开合状态（主进程是唯一权威）：三个窗口据此同步按钮的按下态
  onPanelState: (cb: (state: {
    queueOpen: boolean; terminalOpen: boolean; terminalSpan: boolean;
  }) => void) => {
    const handler = (
      _e: unknown,
      state: { queueOpen: boolean; terminalOpen: boolean; terminalSpan: boolean },
    ) => cb(state);
    ipcRenderer.on('ranarch:panelState', handler);
    return () => ipcRenderer.removeListener('ranarch:panelState', handler);
  },

  // 终端是否横跨到队列下方（选项菜单里的开关；由主进程统一切窗口宽度）
  setTerminalSpan: (span: boolean) => ipcRenderer.send('ranarch:setTerminalSpan', span),

  // 状态栏用：真实 CPU / 内存占用
  sysUsage: () => ipcRenderer.invoke('ranarch:sysUsage'),

  // 清空终端日志（主进程广播给三个窗口，各自清掉自己那份）
  clearLog: () => ipcRenderer.send('ranarch:clearLog'),
  onClearLog: (cb: () => void) => {
    const handler = () => cb();
    ipcRenderer.on('ranarch:clearLog', handler);
    return () => ipcRenderer.removeListener('ranarch:clearLog', handler);
  },

  // 主窗广播队列/进度快照（面板窗据此保持一致）
  syncState: (snapshot: unknown) => ipcRenderer.send('ranarch:sync', snapshot),

  // 面板窗接收主窗广播的快照
  onSyncState: (cb: (snapshot: unknown) => void) => {
    const handler = (_e: unknown, snapshot: unknown) => cb(snapshot);
    ipcRenderer.on('ranarch:sync', handler);
    return () => ipcRenderer.removeListener('ranarch:sync', handler);
  },

  // 面板窗把用户操作回传给主窗（安装流程只在主窗执行）
  sendCommand: (cmd: unknown) => ipcRenderer.send('ranarch:command', cmd),

  // 主窗接收面板窗回传的操作
  onCommand: (cb: (cmd: unknown) => void) => {
    const handler = (_e: unknown, cmd: unknown) => cb(cmd);
    ipcRenderer.on('ranarch:command', handler);
    return () => ipcRenderer.removeListener('ranarch:command', handler);
  },
});
