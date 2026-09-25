// RanArch Installer — 设置模块（含与后端字段的对照文档）
//
// ============================================================================
// 与后端的对照关系（依据 docs/frontend-integration.md 第 6 节）
// ============================================================================
//
// 校验开关有四个入口，优先级从高到低：
//   1. IPC  options.checks   ← 本模块写入（每次安装单独下发）
//   2. CLI  --no-<name>
//   3. /etc/ranarch/ranarch.conf  [checks]  ← 守护进程默认值
//   4. 代码默认值（全部开启，sandbox_trial 例外，见下）
//
// 后端类型：ValidationOptions / InstallOptions（include/ranarch/types.h）
// 序列化：  src/daemon/protocol.h 的 checks_to_json / checks_from_json
// 解析处：  src/daemon/ipc_server.cpp 的 install 分支（opts->find("checks")）
//
// 五个开关的语义与「关闭后的后果」（后端注释原文要点）：
//   signature      关闭 ⇒ 不验签，记录为 skipped（等价 --no-signature）
//   dependencies   关闭 ⇒ 不解析依赖（等价 pacman --nodeps）
//   conflicts      关闭 ⇒ 允许覆盖他人文件（等价 --overwrite）
//   path_traversal 关闭 ⇒ 不再拦截越出根目录的路径，放弃对恶意包的最后防线（危险）
//   sandbox_trial  开启 ⇒ 落盘前额外先做一次沙箱试装（默认关闭，属「额外开启」项）
//
// 注意 sandbox_trial 的方向与其他四项相反：它是 opt-in，默认 false。
// 关闭任一项，守护进程都会发一条 `checks_disabled` 事件，界面上必须显著提示。

/** 与后端 ValidationOptions 一一对应的五个校验开关 */
export interface CheckToggles {
  signature: boolean;
  dependencies: boolean;
  conflicts: boolean;
  path_traversal: boolean;
  sandbox_trial: boolean;
}

/** 本次安装的选项（对应后端 InstallOptions） */
export interface Settings extends CheckToggles {
  /** 对应 InstallOptions.interactive */
  interactive: boolean;
  /** 对应 InstallOptions.dry_run */
  dry_run: boolean;
}

/** 守护进程侧的策略快照，从 /etc/ranarch/ranarch.conf 实际读取 */
export interface DaemonPolicy {
  /** 是否成功读到配置文件 */
  loaded: boolean;
  /** 读到的配置文件路径 */
  configPath: string;
  /** [checks] 段 —— 本次安装未覆盖时生效的默认值 */
  checks: CheckToggles;
  /** [signature] policy */
  signature_policy: 'strict' | 'warn' | 'ignore';
  /** [sandbox] trial_install */
  sandbox_trial_install: boolean;
  /** [sandbox] backend */
  sandbox_backend: 'bwrap' | 'nspawn' | 'none';
  /** [deps] default_action */
  dep_default_action: 'ask' | 'skip' | 'abort';
}

/** 后端代码默认值（对应 types.h 的 ValidationOptions / Config） */
export const DEFAULT_CHECKS: CheckToggles = {
  signature: true,
  dependencies: true,
  conflicts: true,
  path_traversal: true,
  sandbox_trial: false,
};

export const DEFAULT_SETTINGS: Settings = {
  ...DEFAULT_CHECKS,
  interactive: true,
  dry_run: false,
};

export const FALLBACK_POLICY: DaemonPolicy = {
  loaded: false,
  configPath: '/etc/ranarch/ranarch.conf',
  checks: { ...DEFAULT_CHECKS },
  signature_policy: 'warn',
  sandbox_trial_install: true,
  sandbox_backend: 'bwrap',
  dep_default_action: 'ask',
};

/** 危险级别：用于界面警示与徽标提示 */
export type RiskLevel = 'safe' | 'caution' | 'danger';

export interface SettingSpec {
  key: keyof Settings;
  label: string;
  /** 后端字段路径 */
  backendField: string;
  risk: RiskLevel;
  /** 关闭/开启该开关带来的后果 */
  help: string;
  /** 关闭时是否需要二次确认（协议要求 path_traversal 必须确认） */
  confirmOnDisable?: boolean;
  /** 界面上开关的语义方向：true 表示「勾选 = 开启」，sandbox_trial 亦然，
   *  但文案要说明它是 opt-in */
  optIn?: boolean;
}

/** 设置项规格表 —— 界面的唯一数据源 */
export const SETTING_SPECS: SettingSpec[] = [
  {
    key: 'signature',
    label: '签名校验',
    backendField: 'options.checks.signature',
    risk: 'caution',
    help: '验证包的 GPG 签名。关闭后不再读取签名，安装记录标记为 skipped。等价 --no-signature。',
  },
  {
    key: 'dependencies',
    label: '依赖解析',
    backendField: 'options.checks.dependencies',
    risk: 'caution',
    help: '解析并满足依赖。关闭后等同于 pacman --nodeps，可能装出无法运行的软件。',
  },
  {
    key: 'conflicts',
    label: '文件冲突检测',
    backendField: 'options.checks.conflicts',
    risk: 'caution',
    help: '拒绝覆盖已被其他包占用的文件。关闭后允许覆盖，等价 --overwrite，可能破坏系统。',
  },
  {
    key: 'path_traversal',
    label: '路径越界拦截',
    backendField: 'options.checks.path_traversal',
    risk: 'danger',
    help: '拦截试图写出安装根目录的路径（如 ../../etc/passwd）。关闭即放弃对恶意包的最后一道防线，仅在你完全信任该包时使用。',
    confirmOnDisable: true,
  },
  {
    key: 'sandbox_trial',
    label: '沙箱试装',
    backendField: 'options.checks.sandbox_trial',
    risk: 'safe',
    help: '额外项，默认关闭。开启后会在落盘前先做一次沙箱试装，用更慢的安装换取更早发现问题。',
    optIn: true,
  },
  {
    key: 'interactive',
    label: '交互模式',
    backendField: 'options.interactive',
    risk: 'safe',
    help: '开启后遇到未满足的依赖会弹出选择框等你决策。关闭则按守护进程 [deps] default_action 处理（无交互通道时 ask 退化为 skip）。',
  },
  {
    key: 'dry_run',
    label: '只试装不落盘',
    backendField: 'options.dry_run',
    risk: 'safe',
    help: '只做沙箱试装并报告结果，不写入真实系统。适合先探路。',
  },
];

/** 当前处于关闭状态的校验项名称，顺序与后端 disabled_checks() 一致 */
export function disabledChecks(checks: CheckToggles): string[] {
  const out: string[] = [];
  if (!checks.signature) out.push('signature');
  if (!checks.dependencies) out.push('dependencies');
  if (!checks.conflicts) out.push('conflicts');
  if (!checks.path_traversal) out.push('path_traversal');
  return out;
}

/** 是否存在高风险设置（用于设置按钮的警示徽标） */
export function hasRiskySetting(settings: Settings): boolean {
  return !settings.path_traversal;
}

/**
 * 把设置转换为后端 install 请求的 options。
 * 这是设置项到达后端的唯一出口，显式发送全部五项，避免依赖配置漂移。
 */
export function toInstallOptions(settings: Settings): {
  interactive: boolean;
  dry_run: boolean;
  checks: CheckToggles;
} {
  return {
    interactive: settings.interactive,
    dry_run: settings.dry_run,
    checks: {
      signature: settings.signature,
      dependencies: settings.dependencies,
      conflicts: settings.conflicts,
      path_traversal: settings.path_traversal,
      sandbox_trial: settings.sandbox_trial,
    },
  };
}

// ---------- 持久化 ----------

const STORAGE_KEY = 'ranarch.settings.v3';

interface Persisted {
  /** 用户显式改过的项 —— 未改过的项始终跟随守护进程配置 */
  touched: string[];
  values: Partial<Record<keyof Settings, boolean>>;
}

/**
 * 读取设置。
 *
 * 基线是守护进程的 [checks] 真实配置，只有用户显式动过的项才用本地值覆盖。
 * 这样才不会出现「配置里关了沙箱、界面上却一直显示开启」这类不一致 ——
 * 管理员改了 ranarch.conf，界面上没被用户动过的项会跟着变。
 */
export function loadSettings(policy?: DaemonPolicy): Settings {
  const settings: Settings = {
    ...(policy?.checks ?? DEFAULT_CHECKS),
    interactive: DEFAULT_SETTINGS.interactive,
    dry_run: DEFAULT_SETTINGS.dry_run,
  };

  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return settings;
    const saved = JSON.parse(raw) as Persisted;
    if (!Array.isArray(saved?.touched) || !saved.values) return settings;
    for (const key of saved.touched) {
      const v = saved.values[key as keyof Settings];
      if (typeof v === 'boolean') {
        (settings as unknown as Record<string, boolean>)[key] = v;
      }
    }
  } catch {
    // 解析失败则回落守护进程配置
  }
  return settings;
}

export function saveSettings(settings: Settings, touched: Set<string>): void {
  const values: Partial<Record<keyof Settings, boolean>> = {};
  for (const key of touched) {
    if (key in settings) values[key as keyof Settings] = settings[key as keyof Settings];
  }
  const payload: Persisted = { touched: [...touched], values };
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(payload));
  } catch {
    // 存储不可用时忽略（例如隐私模式）
  }
}

const TOUCHED_KEY = 'ranarch.settings.touched.v3';

/** 读取「用户显式改过」的键集合 */
export function loadTouched(): Set<string> {
  try {
    const raw = localStorage.getItem(TOUCHED_KEY);
    if (raw) return new Set(JSON.parse(raw) as string[]);
  } catch {
    // 忽略
  }
  return new Set();
}

export function saveTouched(touched: Set<string>): void {
  try {
    localStorage.setItem(TOUCHED_KEY, JSON.stringify([...touched]));
  } catch {
    // 忽略
  }
}
