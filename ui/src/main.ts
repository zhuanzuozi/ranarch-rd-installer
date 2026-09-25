// RanArch Installer — 主入口
import {
  getBackend, PackageInfo, InstallEvent, PickedFile, ConnectionInfo,
  DependencyPrompt, DepChoice, SessionInfo,
  SyncSnapshot, SyncStep, PanelCommand, PanelState,
  parseVersionFromName, parseFormatFromName, formatSize, toOutcome,
  makePtyDecoder, stripAnsi,
} from './api';
import { store } from './store';
import { lookupError, formatErrorCode, ERROR_INFO } from './errors';
import {
  Settings, DaemonPolicy, SETTING_SPECS, DEFAULT_SETTINGS, DEFAULT_CHECKS,
  loadSettings, saveSettings, loadTouched, saveTouched,
  hasRiskySetting, toInstallOptions, disabledChecks,
} from './settings';

const backend = getBackend();

// ---- 面板模式 ----
// 主窗渲染 stage / 设置 / 菜单栏 / 状态栏；队列窗只渲染队列；终端窗只渲染终端。
// 三个窗口的 DOM 完全相同（同一份 index.html），差异全部由 [data-panel] 驱动。
// html 与 body 都要打标记：面板窗需要把两层都设成透明，否则桌面底色会把窗口填实。
const panelParam = new URLSearchParams(location.search).get('panel');
const panel: 'queue' | 'terminal' | null =
  panelParam === 'queue' || panelParam === 'terminal' ? panelParam : null;
document.documentElement.dataset.panel = panel ?? 'main';
document.body.dataset.panel = panel ?? 'main';
const isMainPanel = panel === null;
const isQueuePanel = panel === 'queue';
const isTerminalPanel = panel === 'terminal';

// ---- DOM refs ----
const $ = <T extends HTMLElement = HTMLElement>(sel: string) => document.querySelector(sel) as T;
const app = $('#app');
const dropZone = $('#dropZone');
const btnPickFile = $('#btnPickFile');
const btnInstallAll = $('#btnInstallAll');
const btnQueueToggle = $('#btnQueueToggle');
const btnTerminalToggle = $('#btnTerminalToggle');
const btnQueueCollapse = $('#btnQueueCollapse');
const btnTerminalCollapse = $('#btnTerminalCollapse');
const queueList = $('#queueList');
const queueCount = $('#queueCount');
const statusQueue = $('#statusQueue');
const terminalBody = $('#terminalBody');
const ptyBody = $('#ptyBody') as HTMLPreElement;
const doneSection = $('#doneSection');
const doneList = $('#doneList');
const doneCount = $('#doneCount');
const stepsStrip = $('#stepsStrip');
const stageDrop = $('#stageDrop');
const stageProgress = $('#stageProgress');
const stageSteps = $('#stageSteps');
const progressTitle = $('#progressTitle');
const progressStep = $('#progressStep');
const progressBarFill = $('#progressBarFill');
const progressCurrent = $('#progressCurrent');
const progressPackageName = $('#progressPackageName');
const progressPercent = $('#progressPercent');
const checksWarning = $('#checksWarning');
const checksWarningText = $('#checksWarningText');
const errorModalOverlay = $('#errorModalOverlay');
const errorModalBody = $('#errorModalBody');
const promptOverlay = $('#promptOverlay');
const promptBody = $('#promptBody');
const promptCountdown = $('#promptCountdown');
const statusConn = $('#statusConn');
const connDot = $('#connDot');
const connText = $('#connText');
const btnSettings = $('#btnSettings');
const settingsBadge = $('#settingsBadge');
const settingsList = $('#settingsList');
const settingsCount = $('#settingsCount');
const btnSettingsCollapse = $('#btnSettingsCollapse');
const btnSettingsReset = $('#btnSettingsReset');
const settingsModule = $('#settingsModule');
const queueModule = $('#queueModule');
const terminalModule = $('#terminalModule');
const menuBar = $('#menuBar');
const infoOverlay = $('#infoOverlay');
const infoTitle = $('#infoTitle');
const infoBody = $('#infoBody');

// ---- 阶段定义（与后端 include/ranarch/types.h 的 step_name() 对齐） ----
const STEP_LABELS: Record<string, string> = {
  parsing: '解析',
  verifying_sig: '验签',
  resolving_deps: '依赖解析',
  conflict_check: '冲突检查',
  sandbox_trial: '沙箱试装',
  dep_install: '安装依赖',
  staging: '暂存',
  installing: '写入',
  recording: '记录',
  done: '完成',
};

/**
 * 后端可能推送的完整阶段顺序，仅用于把「当前阶段」换算成进度比例。
 * 界面上展示的阶段一律来自后端实际推送的事件，不预置任何一项 ——
 * 沙箱试装未启用时后端根本不会推送 sandbox_trial，界面也就不会出现它。
 */
const CANONICAL_STEPS = [
  'parsing', 'verifying_sig', 'resolving_deps', 'conflict_check',
  'sandbox_trial', 'dep_install', 'staging', 'installing', 'recording', 'done',
];

/** 后端校验开关名 → 中文（用于 checks_disabled 警告） */
const CHECK_LABELS: Record<string, string> = {
  signature: '签名校验',
  dependencies: '依赖解析',
  conflicts: '文件冲突检测',
  path_traversal: '路径越界拦截',
};

// ---- 终端日志 ----
function log(text: string, type: string = ''): void {
  const now = new Date().toTimeString().slice(0, 8);
  const line = document.createElement('div');
  line.className = 'term-line';
  line.innerHTML = `<span class="ts">${now}</span><span class="${type}">${escapeHtml(text)}</span>`;
  terminalBody.appendChild(line);
  terminalBody.scrollTop = terminalBody.scrollHeight;
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
}

/** 清空终端日志（每个窗口各有一份日志 DOM，主进程广播后各自清自己那份） */
function clearTerminalLog(): void {
  terminalBody.innerHTML = '';
  ptyBuffer = '';
  ptyBody.textContent = '';
}

/** 复制整份终端日志（LOG.TXT + TTY）到剪贴板 */
async function copyTerminalLog(): Promise<void> {
  const lines = Array.from(terminalBody.querySelectorAll<HTMLElement>('.term-line'))
    .map(el => el.innerText.replace(/\s+$/, ''));
  const pty = (ptyBody.textContent ?? '').trim();
  const text = [lines.join('\n'), pty ? `--- TTY ---\n${pty}` : ''].filter(Boolean).join('\n');
  if (!text) {
    log('终端还没有内容可复制', 'info');
    return;
  }
  try {
    await navigator.clipboard.writeText(text);
    log(`已复制终端日志（${text.length} 字符）`, 'info');
  } catch {
    // 剪贴板 API 在非安全上下文可能不可用，退回 execCommand
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.cssText = 'position:fixed;top:-1000px;opacity:0';
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand('copy');
    ta.remove();
    log(ok ? `已复制终端日志（${text.length} 字符）` : '复制失败：剪贴板不可用', ok ? 'info' : 'err');
  }
}

backend.onClearLog(() => {
  clearTerminalLog();
  if (isMainPanel) log('终端日志已清空', 'warn');
});

// ---- 连接状态指示 ----
function renderConnection(info: ConnectionInfo): void {
  connDot.className = 'conn-dot ' + info.state;
  statusConn.classList.toggle('disconnected', info.state === 'disconnected');

  const labels: Record<string, string> = {
    connecting: '连接中...',
    connected: '守护进程',
    disconnected: '未连接',
  };
  connText.textContent = labels[info.state] ?? info.state;

  const tip = info.error
    ? `${info.state} — ${info.error}\n套接字: ${info.socketPath}`
    : `${info.state}\n套接字: ${info.socketPath}`;
  statusConn.title = tip;
}

// ---- 设置侧栏 ----

/** 当前设置（基线来自守护进程 [checks]，用户改过的项用本地值覆盖） */
let settings: Settings = loadSettings();
/** 用户显式改过的设置键 */
const touchedKeys = loadTouched();
/** 守护进程策略快照（只读展示用） */
let daemonPolicy: DaemonPolicy | null = null;

/**
 * 按 SETTING_SPECS 渲染设置项。
 * 规格表在 src/settings.ts，每项都标注了对应的后端字段。
 */
function renderSettings(): void {
  settingsList.innerHTML = '';

  // 取值来源说明：让用户清楚每一层配置的优先级
  const source = document.createElement('div');
  source.className = 'settings-source';
  const policyPath = daemonPolicy?.configPath ?? '/etc/ranarch/ranarch.conf';
  const policyState = daemonPolicy?.loaded ? '已读取' : '未读到，显示代码默认值';
  source.innerHTML = `开关随 install 请求的 <b>options.checks</b> 下发，优先级高于守护进程配置。<br>`
    + `守护进程配置 <b>${escapeHtml(policyPath)}</b>（${policyState}）`;
  settingsList.appendChild(source);

  const title = document.createElement('div');
  title.className = 'setting-group-title';
  title.textContent = '本次安装选项 · 随请求下发';
  settingsList.appendChild(title);

  for (const spec of SETTING_SPECS) {
    const row = document.createElement('div');
    row.className = 'setting-row';
    const on = settings[spec.key] === true;

    const riskBadge = spec.risk === 'safe' ? ''
      : `<span class="setting-risk ${spec.risk}">${spec.risk === 'danger' ? '危险' : '注意'}</span>`;

    row.innerHTML = `
      <div class="setting-head">
        <span class="setting-label">${spec.label}</span>
        ${riskBadge}
        <input type="checkbox" class="opt-check${spec.risk === 'danger' ? ' danger' : ''}"
               id="opt-${spec.key}" ${on ? 'checked' : ''}>
        <span class="opt-state${on ? (spec.risk === 'danger' ? ' danger' : ' on') : ''}">${on ? '开' : '关'}</span>
      </div>
      <div class="setting-help">${spec.help}</div>
      <div class="setting-field">后端字段 <b>${spec.backendField}</b>${spec.optIn ? ' · <b>opt-in，默认关</b>' : ''}</div>
    `;
    settingsList.appendChild(row);

    const check = row.querySelector<HTMLInputElement>('input.opt-check')!;
    check.addEventListener('change', () => {
      // 协议要求：关闭 path_traversal 必须二次确认
      if (!check.checked && spec.confirmOnDisable) {
        const yes = window.confirm(
          `确认关闭「${spec.label}」？\n\n${spec.help}\n\n这会放弃对恶意安装包的一道防线。`);
        if (!yes) {
          check.checked = true;
          return;
        }
      }

      (settings as unknown as Record<string, boolean>)[spec.key] = check.checked;
      touchedKeys.add(spec.key);
      saveSettings(settings, touchedKeys);
      saveTouched(touchedKeys);

      const stateEl = row.querySelector('.opt-state');
      if (stateEl) {
        stateEl.textContent = check.checked ? '开' : '关';
        stateEl.classList.toggle('on', check.checked && spec.risk !== 'danger');
        stateEl.classList.toggle('danger', check.checked && spec.risk === 'danger');
      }
      updateSettingsBadge();
      log(`设置 ${spec.backendField} = ${check.checked}`, 'cmd');
    });
  }

  // 后端配置里的其余策略（只读展示，帮助用户对照）
  if (daemonPolicy) {
    const title2 = document.createElement('div');
    title2.className = 'setting-group-title';
    title2.textContent = '守护进程策略 · 只读';
    settingsList.appendChild(title2);

    const rows: Array<[string, string, string]> = [
      ['签名策略', 'Config.[signature] policy', daemonPolicy.signature_policy],
      ['沙箱试装', 'Config.[sandbox] trial_install', String(daemonPolicy.sandbox_trial_install)],
      ['沙箱后端', 'Config.[sandbox] backend', daemonPolicy.sandbox_backend],
      ['依赖缺失默认动作', 'Config.[deps] default_action', daemonPolicy.dep_default_action],
    ];
    for (const [label, field, value] of rows) {
      const row = document.createElement('div');
      row.className = 'setting-row reserved';
      row.innerHTML = `
        <div class="setting-head">
          <span class="setting-label">${label}</span>
          <span class="opt-select" style="margin-left:auto">${escapeHtml(value)}</span>
        </div>
        <div class="setting-field">后端字段 <b>${field}</b> · 修改需编辑配置文件后 <b>SIGHUP</b></div>
      `;
      settingsList.appendChild(row);
    }
  }

  const off = disabledChecks(settings).length;
  settingsCount.textContent = off > 0 ? `[${off} 项已关]` : '[全部开启]';
}

function updateSettingsBadge(): void {
  const risky = hasRiskySetting(settings);
  settingsBadge.hidden = !risky;
  btnSettings.title = risky ? '设置 — 路径越界拦截已关闭' : '设置';
}

function setSettingsOpen(open: boolean): void {
  app.classList.toggle('has-settings', open);
  store.setSilent({ settingsOpen: open });
  // 收起时移出 Tab 顺序与无障碍树（width:0 仍会让内部控件可聚焦）
  settingsModule.inert = !open;
  btnSettings.classList.toggle('on', open);
  btnSettings.setAttribute('aria-expanded', String(open));
  if (open) void refreshDaemonPolicy();
}

btnSettings.addEventListener('click', () => setSettingsOpen(!store.get().settingsOpen));
btnSettingsCollapse.addEventListener('click', () => setSettingsOpen(false));
btnSettingsReset.addEventListener('click', () => {
  // 恢复的基准是守护进程配置，而不是前端写死的编译期默认值
  settings = {
    ...(daemonPolicy?.checks ?? DEFAULT_CHECKS),
    interactive: DEFAULT_SETTINGS.interactive,
    dry_run: DEFAULT_SETTINGS.dry_run,
  };
  touchedKeys.clear();
  saveSettings(settings, touchedKeys);
  saveTouched(touchedKeys);
  renderSettings();
  updateSettingsBadge();
  log('设置已恢复为守护进程配置的默认值', 'warn');
});

/** 拉取守护进程配置并刷新设置面板（每次打开设置时重读，便于看到配置改动） */
async function refreshDaemonPolicy(): Promise<void> {
  try {
    const policy = await backend.daemonPolicy();
    daemonPolicy = policy;
    // 未被用户改过的项跟随守护进程配置
    const merged = loadSettings(policy);
    for (const spec of SETTING_SPECS) {
      if (!touchedKeys.has(spec.key)) {
        (settings as unknown as Record<string, boolean>)[spec.key] = merged[spec.key];
      }
    }
    renderSettings();
    updateSettingsBadge();
  } catch (e) {
    log(`读取守护进程配置失败: ${(e as Error).message}`, 'warn');
  }
}

// ---- 阶段轨迹（按后端实际推送渲染） ----
let currentSteps: SyncStep[] = [];

function resetSteps(): void {
  currentSteps = [];
  renderSteps();
}

function pushStep(step: string, detail?: string): void {
  currentSteps.push({ step, detail });
  renderSteps();
}

function renderSteps(): void {
  const lastActive = currentSteps.length - 1;

  // 主区里的阶段轨迹：标签 + 后端给出的 detail（如 "4521 files"）
  if (isMainPanel) {
    stageSteps.innerHTML = currentSteps.map((r, i) => {
      const cls = i < lastActive ? 'done' : i === lastActive ? 'active' : '';
      const label = STEP_LABELS[r.step] || r.step;
      const detail = r.detail ? ` <span class="detail">${escapeHtml(r.detail)}</span>` : '';
      return `<span class="stage-step ${cls}"><span class="dot"></span>${label}${detail}</span>`;
    }).join('');
  }

  // 终端里的紧凑阶段条
  if (isTerminalPanel) {
    stepsStrip.innerHTML = currentSteps.map((r, i) => {
      const cls = i < lastActive ? 'done' : i === lastActive ? 'active' : '';
      const label = STEP_LABELS[r.step] || r.step;
      const node = `<div class="step-node"><div class="step-dot ${cls}"></div>`
        + `<span class="step-label ${cls}">${label}</span></div>`;
      return i < currentSteps.length - 1
        ? node + `<div class="step-line${i < lastActive ? ' done' : ''}"></div>`
        : node;
    }).join('');
  }
}

/** 当前阶段在完整顺序中的位置 → 单个包内的进度比例 */
function packageFraction(currentStep: string): number {
  const idx = CANONICAL_STEPS.indexOf(currentStep);
  if (idx < 0) return 0;
  return idx / (CANONICAL_STEPS.length - 1);
}

// ---- 主界面进度面板 ----
function updateStageProgress(): void {
  const s = store.get();
  if (!s.installing || !s.currentPackage) {
    stageDrop.hidden = false;
    stageProgress.hidden = true;
    return;
  }
  stageDrop.hidden = true;
  stageProgress.hidden = false;

  const total = Math.max(s.sessionTotal, 1);
  const pct = Math.min(100, Math.round(
    ((s.sessionDone + packageFraction(s.currentStep)) / total) * 100));

  progressTitle.textContent = '正在安装';
  progressStep.textContent = STEP_LABELS[s.currentStep] || s.currentStep;
  progressCurrent.textContent = `${Math.min(s.sessionDone + 1, total)} / ${total}`;
  progressPackageName.textContent = s.currentPackage.name;
  progressPercent.textContent = pct + '%';
  progressBarFill.style.width = pct + '%';
}

/** 校验被关闭的显著警告（后端 checks_disabled 事件；警告条在主区内） */
function showChecksDisabled(checks: string[]): void {
  const names = checks.map(c => CHECK_LABELS[c] || c);
  checksWarningText.textContent = `已关闭校验：${names.join('、')} —— 本次安装刻意绕过了这些安全检查`;
  checksWarning.hidden = false;
}

function hideChecksDisabled(): void {
  checksWarning.hidden = true;
  checksWarningText.textContent = '';
}

// ---- 队列渲染（只在队列面板窗里执行；队列已是独立窗口） ----
function renderQueue(): void {
  if (!isQueuePanel) return;
  const s = store.get();
  queueList.innerHTML = '';
  queueCount.textContent = String(s.queue.length);
  statusQueue.textContent = String(s.queue.length);

  const sorted = [...s.queue].sort((a, b) => {
    const order: Record<string, number> = { error: 0, installing: 1, queued: 2, done: 3 };
    return (order[a.status] ?? 9) - (order[b.status] ?? 9);
  });

  sorted.forEach((pkg) => {
    const el = document.createElement('div');
    const isError = pkg.status === 'error';
    el.className = 'queue-item'
      + (pkg.status === 'installing' ? ' installing' : '')
      + (isError ? ' error' : '');
    if (expandedIds.has(pkg.id)) el.classList.add('expanded');
    el.draggable = pkg.status === 'queued';
    el.dataset.id = String(pkg.id);

    if (isError) {
      el.setAttribute('role', 'button');
      el.setAttribute('tabindex', '0');
      el.setAttribute('aria-label', `${pkg.name} 安装失败，查看错误详情`);
      el.addEventListener('keydown', (e: KeyboardEvent) => {
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          void showErrorModal(pkg);
        }
      });
    }

    const sigDot = pkg.sig === 'signed_ok' ? 'ok' : pkg.sig === 'unsigned' ? 'warn' : 'bad';
    const sigTag = pkg.sig === 'signed_ok'
      ? '<span class="sig-tag ok">SIGNED</span>'
      : pkg.sig === 'unsigned'
        ? '<span class="sig-tag warn">UNSIGNED</span>'
        : '<span class="sig-tag bad">BAD SIG</span>';
    const errInfo = isError ? lookupError(pkg.error_code || '') : null;

    el.innerHTML = `
      <div class="queue-handle"><span></span><span></span><span></span></div>
      <div class="pkg-format ${pkg.format}">${pkg.format.toUpperCase()}</div>
      <div class="pkg-info">
        <div class="pkg-name">${escapeHtml(pkg.name)}</div>
        <div class="pkg-meta">
          ${isError
            ? `<span class="sig-dot bad"></span>${errInfo ? formatErrorCode(errInfo) + ' ' + errInfo.title : '安装失败'}`
            : `<span class="sig-dot ${sigDot}"></span>${pkg.version} · ${pkg.size}`}
        </div>
      </div>
      ${pkg.status === 'queued' ? `<button class="pkg-remove" data-remove="${pkg.id}" title="移除">×</button>` : ''}
      <div class="pkg-detail">
        <div class="detail-grid">
          <span class="detail-k">FILE_NAME:</span><span class="detail-v">${escapeHtml(pkg.name)}</span>
          <span class="detail-k">FILE_TYPE:</span><span class="detail-v">${pkg.format === 'deb' ? 'Debian Package' : 'RPM Package'}</span>
          <span class="detail-k">FILE_SIZE:</span><span class="detail-v">${pkg.size}</span>
          <span class="detail-k">VERSION:</span><span class="detail-v">${pkg.version}</span>
          <span class="detail-k">PATH:</span><span class="detail-v">${escapeHtml(pkg.path)}</span>
          <span class="detail-k">SESSION:</span><span class="detail-v">${escapeHtml(pkg.session_id || '—')}</span>
          <span class="detail-k">STATUS:</span><span class="detail-v">${sigTag}</span>
        </div>
      </div>
      <div class="pkg-progress"><div class="pkg-progress-fill"></div></div>
    `;

    if (pkg.status === 'queued') {
      const styleEl = el as HTMLElement;
      el.addEventListener('dragstart', (e: DragEvent) => {
        draggedId = pkg.id;
        el.classList.add('dragging');
        e.dataTransfer!.effectAllowed = 'move';
      });
      el.addEventListener('dragend', () => {
        el.classList.remove('dragging');
        draggedId = null;
        styleEl.style.borderTopColor = '';
        styleEl.style.borderBottomColor = '';
      });
      el.addEventListener('dragover', (e: DragEvent) => {
        e.preventDefault();
        if (!draggedId || draggedId === pkg.id) return;
        const rect = el.getBoundingClientRect();
        const before = e.clientY < rect.top + rect.height / 2;
        styleEl.style.borderTopColor = before ? 'var(--pink-deep)' : '';
        styleEl.style.borderBottomColor = before ? '' : 'var(--pink-deep)';
      });
      el.addEventListener('dragleave', () => {
        styleEl.style.borderTopColor = '';
        styleEl.style.borderBottomColor = '';
      });
      el.addEventListener('drop', (e: DragEvent) => {
        e.preventDefault();
        styleEl.style.borderTopColor = '';
        styleEl.style.borderBottomColor = '';
        if (!draggedId || draggedId === pkg.id) return;
        const rect = el.getBoundingClientRect();
        const before = e.clientY < rect.top + rect.height / 2;
        // 排序由主窗执行（队列状态以主窗为准）
        backend.sendCommand({ cmd: 'reorder', fromId: draggedId, toId: pkg.id, before });
      });
    }

    el.addEventListener('click', (e: MouseEvent) => {
      const target = e.target as HTMLElement;
      if (target.closest('[data-remove]')) return;
      if (isError) {
        void showErrorModal(pkg);
        return;
      }
      if (pkg.status === 'installing') return;
      const expanded = el.classList.toggle('expanded');
      if (expanded) expandedIds.add(pkg.id);
      else expandedIds.delete(pkg.id);
    });

    queueList.appendChild(el);
  });
}

// ---- 已完成列表 ----
function renderDoneList(): void {
  if (!isQueuePanel) return;
  const s = store.get();
  doneCount.textContent = String(s.done.length);
  doneSection.classList.toggle('has-items', s.done.length > 0);
  doneList.innerHTML = s.done.map(p => `
    <div class="done-item">
      <span class="check">✓</span>
      <span class="done-name">${escapeHtml(p.name)}</span>
      <span class="done-size">${p.size}</span>
    </div>
  `).join('');
}

// ---- 失败详情弹层 ----
async function showErrorModal(pkg: PackageInfo): Promise<void> {
  const info = lookupError(pkg.error_code || '');
  errorModalBody.innerHTML = `
    <div class="error-code-badge">${info ? formatErrorCode(info) : 'E???'}</div>
    <div class="error-title">${info?.title || '未知错误'}</div>
    <div class="error-detail">${escapeHtml(info?.detail || pkg.error_message || '发生了未知错误。')}</div>
    ${info?.hint ? `<div class="error-hint">${escapeHtml(info.hint)}</div>` : ''}
    ${pkg.error_message ? `<div class="detail-grid" style="margin-bottom:12px">
      <span class="detail-k">DETAIL:</span><span class="detail-v">${escapeHtml(pkg.error_message)}</span>
      <span class="detail-k">CATEGORY:</span><span class="detail-v">${info?.category || '—'}</span>
      <span class="detail-k">SESSION:</span><span class="detail-v">${escapeHtml(pkg.session_id || '—')}</span>
    </div>` : ''}
    <div id="sessionTrail"></div>
    <div class="error-actions">
      ${info?.retryable ? `<button class="btn95" data-retry>重试</button>` : ''}
      <button class="btn95" data-dismiss>关闭</button>
    </div>
  `;
  errorModalOverlay.hidden = false;

  const retryBtn = errorModalBody.querySelector('[data-retry]');
  if (retryBtn) {
    retryBtn.addEventListener('click', () => {
      errorModalOverlay.hidden = true;
      // 队列状态以主窗为准：面板窗把「重试」回传给主窗执行
      if (isQueuePanel) backend.sendCommand({ cmd: 'retry', id: pkg.id });
      else store.updatePackage(pkg.id, { status: 'queued', error_code: undefined, error_message: undefined });
      log(`已重置为待安装: ${pkg.name}`, 'info');
    });
  }
  errorModalBody.querySelector('[data-dismiss]')!.addEventListener('click', () => {
    errorModalOverlay.hidden = true;
  });

  // 向守护进程回放该会话的事件日志（info 请求）
  if (pkg.session_id) {
    const slot = document.getElementById('sessionTrail');
    try {
      const session: SessionInfo = await backend.info(pkg.session_id);
      if (slot) slot.innerHTML = renderSessionTrail(session);
    } catch (e) {
      if (slot) {
        slot.innerHTML = `<div class="setting-field">会话回放不可用: ${escapeHtml((e as Error).message)}</div>`;
      }
    }
  }
}

/** 把 info 返回的事件日志（每项是一段 JSON 文本）渲染成可读轨迹 */
function renderSessionTrail(session: SessionInfo): string {
  const lines: string[] = [];
  for (const raw of session.events) {
    try {
      const ev = JSON.parse(raw) as InstallEvent;
      if (ev.type === 'step') {
        const label = STEP_LABELS[ev.step || ''] || ev.step || '';
        lines.push(`[${label}]${ev.detail ? ' ' + ev.detail : ''}`);
      } else if (ev.type === 'checks_disabled') {
        lines.push(`!! 关闭校验: ${(ev.checks || []).join(' ')}`);
      } else if (ev.type === 'pty_start') {
        lines.push(`$ ${ev.cmd ?? ''}`);
      } else if (ev.type === 'pty_end') {
        lines.push(`(exit ${ev.exit_code})`);
      }
    } catch {
      // 忽略无法解析的单条事件
    }
  }
  if (lines.length === 0) return '';
  return `<div class="detail-grid" style="margin-bottom:12px">
    <span class="detail-k">后端事件:</span>
    <span class="detail-v">${lines.map(escapeHtml).join('<br>')}</span>
  </div>`;
}

errorModalOverlay.addEventListener('click', (e: MouseEvent) => {
  if (e.target === errorModalOverlay) errorModalOverlay.hidden = true;
});

// ---- 依赖决策弹层（后端 prompt；守护进程正在阻塞等待） ----
let promptTimer: number | null = null;

function showPromptModal(p: DependencyPrompt): void {
  const dep = p.dependency;
  const needsName = !dep.mapped;
  let selected: DepChoice = dep.mapped ? 'install' : 'skip';

  const choiceLabels: Array<[DepChoice, string, boolean]> = [
    ['install', '立刻安装依赖', false],
    ['skip', '跳过该依赖', false],
    ['map', '记住映射', false],
    ['abort', '中止本次安装', true],
  ];

  promptBody.innerHTML = `
    <div class="dep-raw">${escapeHtml(dep.raw_name)}</div>
    <div class="dep-grid">
      <span class="dep-k">映射结果</span>
      <span class="dep-v">${dep.arch_candidate ? escapeHtml(dep.arch_candidate) : '（无对应 Arch 包）'}</span>
      <span class="dep-k">版本要求</span>
      <span class="dep-v">${escapeHtml((dep.op || '') + ' ' + (dep.version || '')).trim() || '—'}</span>
      <span class="dep-k">提示编号</span>
      <span class="dep-v">#${p.prompt_id}</span>
    </div>
    ${needsName
      ? `<div class="dep-hint">映射表里没有这项依赖。选择「立刻安装依赖」或「记住映射」时，你必须填写对应的 Arch 包名。</div>`
      : `<div class="setting-field">选择「记住映射」会把 ${escapeHtml(dep.raw_name)} → ${escapeHtml(dep.arch_candidate)} 写入数据库，下次安装不再询问。</div>`}
    <div class="dep-choices">
      ${choiceLabels.map(([v, label, danger]) =>
        `<button class="dep-choice${danger ? ' danger' : ''}${v === selected ? ' selected' : ''}"
                 data-choice="${v}">${label}</button>`).join('')}
    </div>
    <div class="dep-arch-row">
      <span class="dep-arch-label">Arch 包名</span>
      <input class="dep-arch-input" id="depArchInput" type="text"
             value="${escapeHtml(dep.arch_candidate)}" placeholder="例如 glibc">
    </div>
    <div class="dep-actions">
      <button class="btn95 primary" id="depConfirm">确认</button>
    </div>
  `;
  promptOverlay.hidden = false;

  const archInput = document.getElementById('depArchInput') as HTMLInputElement;
  const confirmBtn = document.getElementById('depConfirm')!;
  const choiceBtns = Array.from(promptBody.querySelectorAll<HTMLButtonElement>('.dep-choice'));

  const syncState = () => {
    choiceBtns.forEach(b => b.classList.toggle('selected', b.dataset.choice === selected));
    const need = selected === 'install' || selected === 'map';
    archInput.disabled = !need;
    archInput.style.opacity = need ? '1' : '0.5';
  };
  choiceBtns.forEach(b => b.addEventListener('click', () => {
    selected = b.dataset.choice as DepChoice;
    syncState();
    if (selected === 'install' || selected === 'map') archInput.focus();
  }));
  syncState();
  if (needsName) archInput.focus();

  const submit = () => {
    const need = selected === 'install' || selected === 'map';
    const archName = need ? archInput.value.trim() : '';
    if (need && !archName) {
      archInput.focus();
      archInput.style.borderColor = 'var(--err)';
      return;
    }
    void sendAnswer(p, selected, archName);
  };

  confirmBtn.addEventListener('click', submit);
  promptBody.addEventListener('keydown', (e: KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      submit();
    }
  });

  // 守护进程最多等 5 分钟，超时按 abort 处理 —— 倒计时让用户知道时限
  let remain = 300;
  if (promptTimer !== null) window.clearInterval(promptTimer);
  promptCountdown.textContent = `剩余 ${remain}s`;
  promptTimer = window.setInterval(() => {
    remain -= 1;
    promptCountdown.textContent = remain > 0 ? `剩余 ${remain}s` : '已超时';
    if (remain <= 0) {
      if (promptTimer !== null) window.clearInterval(promptTimer);
      promptTimer = null;
      promptOverlay.hidden = true;
    }
  }, 1000);
}

async function sendAnswer(p: DependencyPrompt, choice: DepChoice, archName: string): Promise<void> {
  if (promptTimer !== null) {
    window.clearInterval(promptTimer);
    promptTimer = null;
  }
  promptOverlay.hidden = true;
  try {
    await backend.answer(p.session_id, p.prompt_id, choice, archName);
    const label = choice === 'install' ? `安装 ${archName}`
      : choice === 'map' ? `记录映射 → ${archName}`
        : choice === 'abort' ? '中止安装' : '跳过';
    log(`依赖决策 #${p.prompt_id}: ${p.dependency.raw_name} → ${label}`, 'cmd');
  } catch (e) {
    log(`提交依赖决策失败: ${(e as Error).message}`, 'err');
  }
}

// ---- 添加安装包 ----
let draggedId: number | null = null;
/** 队列自动展开只触发一次（此后完全由用户控制） */
let queueAutoExpandUsed = false;
/** 已展开属性详情的条目 id（重渲染后保持） */
const expandedIds = new Set<number>();

async function addFiles(files: PickedFile[]): Promise<void> {
  for (const f of files) {
    store.addToQueue({
      name: f.name,
      format: f.format,
      version: f.version,
      size: f.size,
      path: f.path,
      status: 'queued',
      sig: Math.random() > 0.2 ? 'signed_ok' : 'unsigned',
    });
  }
  if (!queueAutoExpandUsed) {
    queueAutoExpandUsed = true;
    setQueueOpen(true);
  }
  log(`已添加 ${files.length} 个安装包`, 'cmd');
}

/** 从浏览器 File 对象构造 PickedFile（拖放场景） */
function fileToPicked(file: File): PickedFile {
  return {
    path: (file as any).path || file.name,
    name: file.name,
    format: parseFormatFromName(file.name),
    version: parseVersionFromName(file.name),
    size: formatSize(file.size),
  };
}

dropZone.addEventListener('dragover', (e: DragEvent) => {
  e.preventDefault();
  dropZone.classList.add('drag-over');
});
dropZone.addEventListener('dragleave', () => dropZone.classList.remove('drag-over'));
dropZone.addEventListener('drop', async (e: DragEvent) => {
  e.preventDefault();
  dropZone.classList.remove('drag-over');
  const files = Array.from(e.dataTransfer?.files || [])
    .filter(f => /\.(deb|rpm)$/i.test(f.name))
    .map(fileToPicked);
  if (files.length === 0) {
    log('仅支持 .deb / .rpm 文件', 'warn');
    return;
  }
  await addFiles(files);
});

btnPickFile.addEventListener('click', async () => {
  const files = await backend.pickFiles();
  if (files.length === 0) return;
  await addFiles(files);
});

// ---- 面板开关 ----
//
// 队列 / 终端都是独立窗口：这里只发 IPC 开合面板窗，并维护本窗口的 UI 状态
// （菜单栏按钮的按下态、面板窗的 inert）。绝不再做任何窗口尺寸相关的计算。
function setQueueOpen(open: boolean): void {
  store.setSilent({ queueOpen: open });
  if (isQueuePanel) queueModule.inert = !open;
  else syncToggles();
  backend.toggleQueue(open);
}
function setTerminalOpen(open: boolean): void {
  store.setSilent({ terminalOpen: open });
  if (isTerminalPanel) terminalModule.inert = !open;
  else syncToggles();
  backend.toggleTerminal(open);
}
function syncToggles(): void {
  if (!isMainPanel) return;
  const s = store.get();
  btnQueueToggle.classList.toggle('on', s.queueOpen);
  btnQueueToggle.textContent = s.queueOpen ? '队列 ◀' : '队列 ▶';
  btnTerminalToggle.classList.toggle('on', s.terminalOpen);
  btnTerminalToggle.textContent = s.terminalOpen ? '终端 ▲' : '终端 ▼';
}
btnQueueToggle.addEventListener('click', () => setQueueOpen(!store.get().queueOpen));
btnQueueCollapse.addEventListener('click', () => setQueueOpen(false));
btnTerminalToggle.addEventListener('click', () => setTerminalOpen(!store.get().terminalOpen));
btnTerminalCollapse.addEventListener('click', () => setTerminalOpen(false));

// ---- 菜单栏（文件 / 选项 / 帮助） ----
//
// 菜单只在主窗里生效：三个窗口共用同一份 index.html，菜单栏在面板窗里是 display:none，
// 而且「一键安装」这类流程只允许跑一份，所以这里全部用 isMainPanel 兜住。
const menuItems = Array.from(
  menuBar.querySelectorAll<HTMLElement>('.menu-item'),
);
let openMenuName: string | null = null;

/**
 * 终端是否横跨到队列下方。
 *
 * 纯界面偏好（不属于下发给后端的安装选项），存在本地；窗口宽度由主进程按这个开关算，
 * 所以改动要走 IPC 让主进程重新摆放窗口，并广播给三个窗口保持一致。
 */
const SPAN_STORAGE_KEY = 'ranarch.ui.terminalSpan';
let terminalSpan = localStorage.getItem(SPAN_STORAGE_KEY) === '1';

function setTerminalSpan(span: boolean, feedback = true): void {
  terminalSpan = span;
  try {
    localStorage.setItem(SPAN_STORAGE_KEY, span ? '1' : '0');
  } catch { /* 隐私模式等写不进去就算了，本次会话仍然生效 */ }
  backend.setTerminalSpan(span);
  syncMenuState();
  if (feedback) {
    log(span
      ? '终端将在队列展开时横跨其下方（窗口变宽）'
      : '终端固定为主区宽度，不再随队列变宽', 'info');
  }
}

function menuCommand(cmd: string): HTMLButtonElement | null {
  return menuBar.querySelector<HTMLButtonElement>(`.menu-command[data-cmd="${cmd}"]`);
}

function setMenuOpen(name: string | null): void {
  openMenuName = name;
  for (const item of menuItems) {
    const on = item.dataset.menu === name;
    item.classList.toggle('open', on);
    item.setAttribute('aria-expanded', String(on));
    const dropdown = item.querySelector<HTMLElement>('.menu-dropdown');
    if (dropdown) dropdown.hidden = !on;
  }
  if (name) syncMenuState();
}

/** 菜单项的勾选 / 禁用态每次展开时按真实状态刷新 */
function syncMenuState(): void {
  const s = store.get();
  menuCommand('toggle-queue')?.classList.toggle('is-checked', s.queueOpen);
  menuCommand('toggle-terminal')?.classList.toggle('is-checked', s.terminalOpen);
  menuCommand('toggle-span')?.classList.toggle('is-checked', terminalSpan);
  const installAll = menuCommand('install-all');
  if (installAll) installAll.disabled = s.installing || s.queue.length === 0;
  const clearQueue = menuCommand('clear-queue');
  if (clearQueue) clearQueue.disabled = s.queue.length === 0;
}

async function runMenuCommand(cmd: string): Promise<void> {
  switch (cmd) {
    case 'pick': {
      const files = await backend.pickFiles();
      if (files.length) await addFiles(files);
      break;
    }
    case 'install-all':
      void runInstallAll();
      break;
    case 'clear-queue': {
      // 正在安装的项不动（它已经和后端会话绑上了），其余全部移出队列
      const items = store.get().queue;
      const removable = items.filter(p => p.status !== 'installing');
      removable.forEach(p => store.removeFromQueue(p.id));
      const kept = items.length - removable.length;
      if (removable.length === 0 && kept === 0) log('队列里没有待安装的项目', 'info');
      else log(`已清空队列：移除 ${removable.length} 项${kept ? `，保留 ${kept} 项正在安装` : ''}`, 'warn');
      break;
    }
    case 'quit':
      backend.close();
      break;
    case 'toggle-queue':
      setQueueOpen(!store.get().queueOpen);
      break;
    case 'toggle-terminal':
      setTerminalOpen(!store.get().terminalOpen);
      break;
    case 'toggle-span':
      setTerminalSpan(!terminalSpan);
      break;
    case 'settings':
      setSettingsOpen(true);
      break;
    case 'reload-policy':
      await refreshDaemonPolicy();
      log('已重新读取守护进程配置', 'info');
      break;
    case 'copy-log':
      await copyTerminalLog();
      break;
    case 'clear-log':
      backend.clearLog();
      break;
    case 'shortcuts':
      showShortcuts();
      break;
    case 'error-codes':
      showErrorCodes();
      break;
    case 'about':
      showAbout();
      break;
  }
}

function handleMenuClick(e: MouseEvent): void {
  const target = e.target as HTMLElement;
  const command = target.closest<HTMLElement>('.menu-command');
  if (command) {
    setMenuOpen(null);
    if (command.dataset.cmd) void runMenuCommand(command.dataset.cmd);
    return;
  }
  const item = target.closest<HTMLElement>('.menu-item');
  if (!item) return;
  const name = item.dataset.menu;
  if (name) setMenuOpen(openMenuName === name ? null : name);
}

/** 与 Win95 / PC-98 一致：菜单已经展开时，鼠标滑过其他菜单直接切换 */
function handleMenuHover(e: MouseEvent): void {
  if (!openMenuName) return;
  const item = (e.target as HTMLElement).closest<HTMLElement>('.menu-item');
  if (item?.dataset.menu && item.dataset.menu !== openMenuName) setMenuOpen(item.dataset.menu);
}

function handleMenuKeys(e: KeyboardEvent): void {
  if (!infoOverlay.hidden && e.key === 'Escape') {
    e.preventDefault();
    closeInfo();
    return;
  }
  if (e.key === 'Escape' && openMenuName) {
    e.preventDefault();
    const item = menuItems.find(i => i.dataset.menu === openMenuName);
    setMenuOpen(null);
    item?.focus();
    return;
  }
  // Alt+F / O / H 打开对应菜单（与菜单标题里的 (F)(O)(H) 对应）
  if (e.altKey && !e.ctrlKey && !e.metaKey) {
    const name = { f: 'file', o: 'options', h: 'help' }[e.key.toLowerCase()];
    if (name) {
      e.preventDefault();
      setMenuOpen(openMenuName === name ? null : name);
      return;
    }
  }
  // 菜单展开时用 ↑ ↓ 在可用命令之间移动
  if (openMenuName && (e.key === 'ArrowDown' || e.key === 'ArrowUp')) {
    const cmds = Array.from(
      menuItems
        .find(i => i.dataset.menu === openMenuName)
        ?.querySelectorAll<HTMLButtonElement>('.menu-command:not(:disabled)') ?? [],
    );
    if (cmds.length) {
      const cur = cmds.indexOf(document.activeElement as HTMLButtonElement);
      const step = e.key === 'ArrowDown' ? 1 : -1;
      cmds[(cur + step + cmds.length) % cmds.length].focus();
      e.preventDefault();
      return;
    }
  }
  const ctrl = e.ctrlKey || e.metaKey;
  if (ctrl && !e.altKey) {
    // Ctrl+C 是浏览器自带的「复制选中内容」，这里只额外提供复制整份日志
    if (e.shiftKey && (e.key === 'C' || e.key === 'c')) {
      e.preventDefault();
      void runMenuCommand('copy-log');
      return;
    }
    if (!e.shiftKey && (e.key === 'o' || e.key === 'O')) {
      e.preventDefault();
      void runMenuCommand('pick');
      return;
    }
    if (e.key === '1') {
      e.preventDefault();
      void runMenuCommand('toggle-queue');
      return;
    }
    if (e.key === '2') {
      e.preventDefault();
      void runMenuCommand('toggle-terminal');
      return;
    }
    if (e.key === ',') {
      e.preventDefault();
      void runMenuCommand('settings');
      return;
    }
  }
  // F5 在这里是「一键安装」，不是刷新页面
  if (e.key === 'F5') {
    e.preventDefault();
    void runMenuCommand('install-all');
  }
}

// ---- 帮助弹层内容 ----

function showInfo(title: string, html: string): void {
  infoTitle.textContent = title;
  infoBody.innerHTML = html;
  infoOverlay.hidden = false;
}

function closeInfo(): void {
  infoOverlay.hidden = true;
  infoBody.innerHTML = '';
}

function infoSection(title: string, bodyHtml: string): string {
  return `<div class="info-section"><div class="info-section-title">${title}</div>${bodyHtml}</div>`;
}

function kbd(keys: string): string {
  return keys.split('+').map(k => `<span class="info-kbd">${escapeHtml(k)}</span>`).join('');
}

function showShortcuts(): void {
  const groups: Array<[string, Array<[string, string]>]> = [
    ['安装流程', [
      ['Ctrl+O', '选择安装包（可多选）'],
      ['F5', '一键安装队列里的全部包'],
      ['Enter', '依赖决策弹层：确认当前选择'],
    ]],
    ['面板与设置', [
      ['Ctrl+1', '开合队列面板'],
      ['Ctrl+2', '开合终端面板'],
      ['Ctrl+,', '打开安装设置'],
    ]],
    ['控制台', [
      ['Ctrl+C', '复制选中的日志内容'],
      ['Ctrl+Shift+C', '复制整份终端日志'],
    ]],
    ['菜单', [
      ['Alt+F', '文件菜单'],
      ['Alt+O', '选项菜单'],
      ['Alt+H', '帮助菜单'],
      ['↑/↓', '在菜单项之间移动'],
      ['Esc', '关闭菜单 / 本弹层'],
    ]],
  ];
  const html = `<div class="info-lead">菜单也可以用键盘操作：Alt+F / O / H 展开，↑ ↓ 移动，Enter 执行，Esc 收起。</div>` +
    groups.map(([title, rows]) => `
    <div class="info-section-title" style="margin-top:12px">${title}</div>
    ${rows.map(([k, desc]) => `<div class="info-row"><div class="info-key">${kbd(k)}</div><div class="info-val">${escapeHtml(desc)}</div></div>`).join('')}
  `).join('');
  showInfo('快捷键一览', html);
}

function showErrorCodes(): void {
  // 直接用错误码表渲染：后端回传 error_code 时这里就是权威对照
  const byCategory = new Map<string, Array<[string, typeof ERROR_INFO[string]]>>();
  for (const [name, info] of Object.entries(ERROR_INFO)) {
    const list = byCategory.get(info.category) ?? [];
    list.push([name, info]);
    byCategory.set(info.category, list);
  }
  const html = `<div class="info-lead">按类别分组。带「可重试」标记的错误可以直接在失败详情里点重试，其余需要先处理掉原因。</div>` +
    Array.from(byCategory.entries()).map(([category, list]) => `
    <div class="info-section-title" style="margin-top:12px">${escapeHtml(category)}</div>
    ${list.sort((a, b) => a[1].code - b[1].code).map(([name, info]) => `
      <div class="code-item">
        <div class="code-head">
          <span class="code-num">${info.code}</span>
          <span class="code-title">${escapeHtml(info.title)}</span>
          ${info.retryable ? '<span class="code-tag">可重试</span>' : ''}
          <span class="code-name">${escapeHtml(name)}</span>
        </div>
        <div class="code-detail">${escapeHtml(info.detail)}</div>
        ${info.hint ? `<div class="code-hint">→ ${escapeHtml(info.hint)}</div>` : ''}
      </div>`).join('')}
  `).join('');
  showInfo('错误码对照表', html);
}

/**
 * 关于：经典样式 —— 左边一枚图标，右边「名称 / 版本 / 作者 / 日期」四行，
 * 下面一条分隔线给出当前与守护进程的连接情况。
 * 版本号取自 package.json、日期取打包当天，都由 Vite 在构建时注入，不会写死过期。
 */
function showAbout(): void {
  const conn = store.get().connection;
  const stateText = conn.state === 'connected' ? '已连接守护进程'
    : conn.state === 'connecting' ? '正在连接守护进程' : '未连接守护进程';
  const row = (key: string, value: string) =>
    `<div class="about-row"><span class="about-key">${key}</span><span class="about-val">${value}</span></div>`;
  const html = `
    <div class="about-box">
      <img class="about-icon" src="./icon.png" alt="RanArch Installer">
      <div class="about-info">
        <div class="about-app">RANARCH INSTALLER</div>
        ${row('版本', escapeHtml(__APP_VERSION__))}
        ${row('作者', '转作子')}
        ${row('日期', escapeHtml(__BUILD_DATE__))}
      </div>
    </div>
    <div class="about-sep"></div>
    <div class="about-foot">${stateText}${conn.socketPath ? ` · ${escapeHtml(conn.socketPath)}` : ''}</div>
  `;
  showInfo('关于 RANARCH INSTALLER', html);
}

if (isMainPanel) {
  menuBar.addEventListener('click', handleMenuClick);
  menuBar.addEventListener('mouseover', handleMenuHover);
  document.addEventListener('keydown', handleMenuKeys);
  // 点菜单栏以外的地方收起菜单（菜单内部的点击已在 handleMenuClick 里处理）
  document.addEventListener('click', (e: MouseEvent) => {
    if (!openMenuName) return;
    if ((e.target as HTMLElement).closest('.menu-bar')) return;
    setMenuOpen(null);
  });
  $('#btnCloseInfo').addEventListener('click', closeInfo);
  infoOverlay.addEventListener('click', (e: MouseEvent) => {
    if (e.target === infoOverlay) closeInfo();
  });
}

// ---- 跨窗口状态同步 ----
//
// 队列内容与安装进度是渲染进程的本地状态（不在后端事件流里）：主窗是唯一权威来源，
// 每次变化都把快照广播给其余窗口；面板窗上的操作则通过命令回传给主窗执行。
// 三个窗口的尺寸全程恒定，同步只涉及数据，不涉及任何窗口几何。

/** 主窗广播队列/进度快照 */
function pushSnapshot(): void {
  if (!isMainPanel) return;
  const s = store.get();
  backend.syncState({
    queue: s.queue,
    done: s.done,
    installing: s.installing,
    sessionTotal: s.sessionTotal,
    sessionDone: s.sessionDone,
    currentStep: s.currentStep,
    currentPackage: s.currentPackage,
    steps: currentSteps,
  });
}

/** 面板窗用主窗的快照覆盖本地共享字段（不触碰各窗口自己的 UI 状态） */
function applySnapshot(snap: SyncSnapshot): void {
  currentSteps = snap.steps ?? [];
  store.set({
    queue: snap.queue ?? [],
    done: snap.done ?? [],
    installing: snap.installing === true,
    sessionTotal: snap.sessionTotal ?? 0,
    sessionDone: snap.sessionDone ?? 0,
    currentStep: snap.currentStep ?? 'idle',
    currentPackage: snap.currentPackage ?? null,
  });
  renderSteps();
  if (isQueuePanel) {
    // 一键安装按钮的状态跟随主窗正在跑的安装会话
    btnInstallAll.toggleAttribute('disabled', snap.installing === true);
    btnInstallAll.textContent = snap.installing ? '安装中...' : '一键安装(A)';
  }
}

/** 主窗执行面板窗回传的操作（安装流程只在主窗里有一份） */
function executePanelCommand(cmd: PanelCommand): void {
  if (!isMainPanel) return;
  switch (cmd.cmd) {
    case 'installAll':
      void runInstallAll();
      break;
    case 'remove': {
      const pkg = store.get().queue.find(p => p.id === cmd.id);
      store.removeFromQueue(cmd.id);
      if (pkg) log(`已移除: ${pkg.name}`, 'warn');
      break;
    }
    case 'reorder':
      store.reorderQueue(cmd.fromId, cmd.toId, cmd.before);
      break;
    case 'retry': {
      const pkg = store.get().queue.find(p => p.id === cmd.id);
      store.updatePackage(cmd.id, { status: 'queued', error_code: undefined, error_message: undefined });
      if (pkg) log(`已重置为待安装: ${pkg.name}`, 'info');
      break;
    }
  }
}

if (isMainPanel) {
  backend.onCommand(executePanelCommand);
  // 面板开合由主进程裁决（面板窗上的「收起」按钮也会改变它）：主窗据此同步菜单按钮
  // 的按下态，保证三个窗口上的状态永远一致。
  backend.onPanelState((state) => {
    store.setSilent({ queueOpen: state.queueOpen, terminalOpen: state.terminalOpen });
    syncToggles();
  });
} else {
  // 面板窗：窗口位置由主进程摆好，这里只负责把内容从共享边滑出 / 收入
  // （终端横跨宽度由主进程改窗口尺寸实现，内容铺满窗口即可）
  const panelRoot = isQueuePanel ? queueModule : terminalModule;
  backend.onPanelState((state) => {
    const open = isQueuePanel ? state.queueOpen : state.terminalOpen;
    // 双 rAF：先让浏览器把「收入态」（内容在窗外）画出来，再切类，
    // 这样窗口刚显示的那一帧内容确实在窗外，滑出动画才是从边缘长出来的。
    requestAnimationFrame(() => requestAnimationFrame(() => {
      panelRoot.classList.toggle('panel-visible', open);
      panelRoot.inert = !open;
    }));
  });
  backend.onSyncState(applySnapshot);
}

// ---- 移除队列项（队列窗把操作回传给主窗执行） ----
queueList.addEventListener('click', (e: MouseEvent) => {
  if (!isQueuePanel) return;
  const btn = (e.target as HTMLElement).closest('[data-remove]');
  if (!btn) return;
  backend.sendCommand({ cmd: 'remove', id: Number(btn.getAttribute('data-remove')) });
});

// ---- 一键安装（安装流程只在主窗里跑一份） ----
let installRunning = false;

async function runInstallAll(): Promise<void> {
  if (installRunning) return;

  const conn = store.get().connection;
  if (conn.state !== 'connected') {
    log(`无法安装：未连接到守护进程（${conn.socketPath || '套接字未知'}）`, 'err');
    log('请确认 ranarch-daemon 正在运行，且当前用户在 ranarch 组内', 'warn');
    setTerminalOpen(true);
    return;
  }

  const queued = store.get().queue.filter(p => p.status === 'queued');
  if (queued.length === 0) {
    log('没有待安装的软件包', 'warn');
    return;
  }

  installRunning = true;
  btnInstallAll.textContent = '安装中...';
  btnInstallAll.setAttribute('disabled', 'true');

  log(`=== INSTALL SESSION START (${queued.length} 个包) ===`, 'cmd');
  const off = disabledChecks(settings);
  if (off.length > 0) log(`注意：本次安装关闭了 ${off.join(' / ')}`, 'warn');

  store.beginSession(queued.length);

  for (const pkg of queued) {
    if (!store.get().installing) break;
    await installOne(pkg);
  }

  store.endSession();
  log('=== SESSION END ===', 'cmd');
  installRunning = false;
  btnInstallAll.textContent = '一键安装(A)';
  btnInstallAll.removeAttribute('disabled');
  renderQueue();
}

if (isMainPanel) {
  btnInstallAll.addEventListener('click', () => void runInstallAll());
} else if (isQueuePanel) {
  // 队列窗不持有安装流程，点击回传给主窗
  btnInstallAll.addEventListener('click', () => backend.sendCommand({ cmd: 'installAll' }));
}

async function installOne(pkg: PackageInfo): Promise<void> {
  hideChecksDisabled();
  resetSteps();
  store.startPackage(pkg.id, '');
  store.updatePackageSilent(pkg.id, { status: 'installing' });
  renderQueue();
  log(`> INSTALL ${pkg.name}`, 'cmd');

  // 设置侧栏里的开关随本次请求下发（字段对照见 src/settings.ts）
  const options = toInstallOptions(settings);
  log(`  选项: checks=${JSON.stringify(options.checks)}`, 'cmd');

  const result = await new Promise<InstallEvent | null>((resolve) => {
    let settled = false;
    const handler = (ev: Event) => {
      if (settled) return;
      settled = true;
      store.removeEventListener('done', handler);
      resolve((ev as CustomEvent).detail as InstallEvent);
    };
    store.addEventListener('done', handler);
    setTimeout(() => {
      if (settled) return;
      settled = true;
      store.removeEventListener('done', handler);
      resolve(null);
    }, 300000); // 大包安装耗时较长，给 5 分钟

    backend.install(pkg.path, options).catch((err) => {
      if (settled) return;
      settled = true;
      store.removeEventListener('done', handler);
      resolve({
        type: 'done', ok: false, summary: '调用失败',
        installed_files_count: 0,
        error_code: 'IO',
        error_message: String(err?.message ?? err),
      });
    });
  });

  if (!result) {
    log('  ✗ 未收到后端结果（超时）', 'err');
    store.finishPackage(pkg.id, false, 'IPC_SESSION_NOT_FOUND', '等待安装结果超时');
  } else {
    const outcome = toOutcome(result);
    if (outcome.ok) {
      log(`  ✓ ${outcome.summary || '安装完成'} (${outcome.installed_files_count} 个文件)`, 'info');
      const el = queueList.querySelector<HTMLElement>(`[data-id="${pkg.id}"]`);
      if (el) {
        el.classList.add('slide-out');
        await new Promise(r => setTimeout(r, 460));
      }
      store.finishPackage(pkg.id, true);
    } else {
      const code = outcome.error_code || 'INTERNAL';
      const msg = outcome.error_message || '安装未完成';
      log(`  ✗ ${code}: ${msg}`, 'err');
      store.finishPackage(pkg.id, false, code, msg);
    }
  }
  renderQueue();
  renderDoneList();
}

// ---- 后端事件订阅 ----
const ptyDecoder = makePtyDecoder();
let ptyBuffer = '';

backend.onEvent((ev: InstallEvent) => {
  // 0) 连接状态变化（由主进程推送给所有窗口）
  if (ev.type === 'conn') {
    const info: ConnectionInfo = {
      state: ev.state ?? 'disconnected',
      socketPath: ev.socketPath ?? '',
      error: ev.detail,
    };
    const prev = store.get().connection.state;
    store.setConnection(info);
    if (info.state === 'connected' && prev !== 'connected') {
      log(`已连接守护进程: ${info.socketPath}`, 'info');
      // 设置面板与安装记录只有主窗用得到
      if (isMainPanel) {
        void loadDaemonPackages();
        void refreshDaemonPolicy();
      }
    } else if (info.state === 'disconnected' && prev === 'connected') {
      log(`与守护进程断开: ${info.error ?? ''}`, 'err');
    }
    return;
  }

  // 1) 会话建立：记录 session_id（失败时用于回放事件日志）
  if (ev.type === 'ok') {
    const cur = store.get().currentPackage;
    if (cur && ev.session_id) {
      store.updatePackageSilent(cur.id, { session_id: ev.session_id });
      store.setSilent({ currentSession: ev.session_id });
      log(`  会话 ${ev.session_id}`, 'cmd');
    }
    return;
  }

  // 2) 有校验被关闭 —— 必须显著提示（警告条在主窗，日志每个窗口都记）
  if (ev.type === 'checks_disabled') {
    const checks = ev.checks ?? [];
    if (isMainPanel) showChecksDisabled(checks);
    log(`!! 校验被关闭: ${checks.join(' ')}`, 'err');
    return;
  }

  // 3) 依赖决策：守护进程正在阻塞等待，必须尽快回答
  if (ev.type === 'prompt') {
    setTerminalOpen(true);
    log(`  需要决策: ${ev.dependency?.raw_name ?? ''}`, 'warn');
    // 弹层只出现在主窗（唯一持有安装流程的窗口），避免三个窗口各弹一个
    if (isMainPanel) showPromptModal(ev as unknown as DependencyPrompt);
    return;
  }

  // 4) 子进程实时终端（pty_data 的 data 是 base64）
  if (ev.type === 'pty_start') {
    ptyBuffer += `\n--- ${ev.cmd ?? ''} ---\n`;
    ptyBody.textContent = ptyBuffer;
    ptyBody.scrollTop = ptyBody.scrollHeight;
    log(`$ ${ev.cmd ?? ''}`, 'cmd');
    return;
  }
  if (ev.type === 'pty_data') {
    ptyBuffer += stripAnsi(ptyDecoder(ev.data ?? ''));
    ptyBody.textContent = ptyBuffer;
    ptyBody.scrollTop = ptyBody.scrollHeight;
    return;
  }
  if (ev.type === 'pty_end') {
    ptyBuffer += `--- exit ${ev.exit_code ?? ''} ---\n`;
    ptyBody.textContent = ptyBuffer;
    ptyBody.scrollTop = ptyBody.scrollHeight;
    log(`(exit ${ev.exit_code ?? ''})`, ev.exit_code === 0 ? 'info' : 'err');
    return;
  }

  // 5) 常规事件
  store.handleEvent(ev);

  if (ev.type === 'step' && ev.step) {
    if (isMainPanel) {
      pushStep(ev.step, ev.detail);
      updateStageProgress();
      // 步骤条与阶段进度属于主窗状态，同步给终端面板窗
      pushSnapshot();
    }
    const label = STEP_LABELS[ev.step] || ev.step;
    log(`${label}${ev.detail ? ': ' + ev.detail : ''}`, 'info');
  }
  if (ev.type === 'error') {
    log(`后端错误 ${ev.code || ''}: ${ev.message || ''}`, 'err');
  }
});

/** 读取守护进程 DB 中已记录的软件包数量（连通性验证 + 信息展示） */
async function loadDaemonPackages(): Promise<void> {
  try {
    const pkgs = await backend.list();
    log(pkgs.length > 0
      ? `守护进程记录 ${pkgs.length} 个已安装软件包`
      : '守护进程记录为空', pkgs.length > 0 ? 'info' : 'cmd');
  } catch (e) {
    log(`读取安装记录失败: ${(e as Error).message}`, 'warn');
  }
}

// ---- Store 订阅 ----
store.addEventListener('change', () => {
  if (isQueuePanel) {
    // 队列面板窗：渲染队列与已完成列表
    renderQueue();
    renderDoneList();
  }
  if (isMainPanel) {
    // 主窗：主区进度 + 状态栏队列计数，并把快照广播给面板窗
    updateStageProgress();
    statusQueue.textContent = String(store.get().queue.length);
    pushSnapshot();
  }
});
store.addEventListener('conn', (e: Event) => {
  if (!isMainPanel) return; // 连接指示在状态栏里，只有主窗有状态栏
  renderConnection((e as CustomEvent).detail as ConnectionInfo);
});

// ---- 终端标签切换（日志 / TTY；只在终端面板窗初始化） ----
if (isTerminalPanel) {
  document.querySelectorAll<HTMLElement>('.terminal-tab').forEach(tab => {
    tab.addEventListener('click', () => {
      document.querySelectorAll('.terminal-tab').forEach(t => t.classList.remove('active'));
      tab.classList.add('active');
      const isTty = tab.dataset.tab === 'tty';
      terminalBody.hidden = isTty;
      ptyBody.hidden = !isTty;
    });
  });
}

// ---- 状态栏时钟 / CPU / 内存（只有主窗有状态栏） ----
if (isMainPanel) {
  setInterval(() => {
    const el = document.getElementById('statusTime');
    if (el) el.textContent = 'SYSTEM_TIME: ' + new Date().toTimeString().slice(0, 8);
  }, 1000);

  // 真实占用率：主进程读 /proc/stat（两次采样差值）与 /proc/meminfo，
  // 这里只负责把数字画出来 —— 条形宽度由 CSS transition 平滑过渡。
  const cpuFill = document.getElementById('cpuFill');
  const cpuText = document.getElementById('cpuText');
  const memFill = document.getElementById('memFill');
  const memText = document.getElementById('memText');
  const memCell = document.getElementById('memCell');
  const paintMeter = (fill: HTMLElement | null, text: HTMLElement | null, v: number) => {
    const pct = Math.max(0, Math.min(100, Math.round(v)));
    if (fill) fill.style.width = pct + '%';
    if (text) text.textContent = pct + '%';
  };
  const pollUsage = async (): Promise<void> => {
    try {
      const usage = await backend.sysUsage();
      paintMeter(cpuFill, cpuText, usage.cpu);
      paintMeter(memFill, memText, usage.mem.percent);
      if (memCell) {
        memCell.title = `内存占用：${usage.mem.usedMb} MB / ${usage.mem.totalMb} MB`;
      }
    } catch {
      // 采样失败（非 Linux 等）就保持上一次的数字，不清零误导用户
    }
  };
  void pollUsage();
  setInterval(() => void pollUsage(), 2000);
}

// ---- 窗口按钮（只有主窗有标题栏） ----
if (isMainPanel) {
  document.getElementById('btnMinimize')?.addEventListener('click', () => backend.minimize());
  document.getElementById('btnMaximize')?.addEventListener('click', () => backend.maximize());
  document.getElementById('btnClose')?.addEventListener('click', () => backend.close());
}

// 浏览器 Mock 环境无法真正关窗，给出明确提示
window.addEventListener('ranarch:mock-close', () => {
  log('关闭窗口：浏览器环境不支持，请在 Electron 中运行', 'warn');
});

// ---- 初始化（按当前窗口承载的面板选择性初始化） ----
log('RANARCH INSTALLER v1.0.4 - dream_core', 'cmd');

if (isMainPanel) {
  syncToggles();
  renderSettings();
  updateSettingsBadge();
  updateStageProgress();
  renderSteps();
  // 把本地的「终端横跨队列」偏好交给主进程 —— 终端窗口宽度按它算
  setTerminalSpan(terminalSpan, false);
  // 让面板窗（包括此刻才加载完的）拿到当前队列/进度状态
  pushSnapshot();

  void backend.status()
    .then(async (info) => {
      store.setConnection(info);
      renderConnection(info);
      if (info.state === 'connected') {
        log(`已连接守护进程: ${info.socketPath}`, 'info');
        await refreshDaemonPolicy();
        await loadDaemonPackages();
      } else {
        log(`等待守护进程: ${info.socketPath}`, 'cmd');
      }
    })
    .catch((e: Error) => {
      const info: ConnectionInfo = { state: 'disconnected', socketPath: '—', error: e.message };
      store.setConnection(info);
      renderConnection(info);
      log(`无法查询连接状态: ${e.message}`, 'err');
    });
} else if (isQueuePanel) {
  renderQueue();
  renderDoneList();
}
