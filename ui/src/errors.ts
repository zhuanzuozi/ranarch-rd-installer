// RanArch Installer — 错误码中文映射
//
// 键与后端 include/ranarch/error.h 的 to_string(RanArchError) 输出一致，
// 即 IPC `done.error_code` / `error.code` 里实际出现的 SCREAMING_SNAKE_CASE
// 字符串（如 "DEP_UNRESOLVED"）。

export interface ErrorInfo {
  code: number;
  category: string;
  title: string;
  detail: string;
  hint: string;
  retryable: boolean;
  showConflicts?: boolean;
  showDeps?: boolean;
}

export const ERROR_INFO: Record<string, ErrorInfo> = {
  // ---- Parse 100 ----
  PARSE_BAD_MAGIC: { code: 100, category: '解析', title: '无法识别的文件', detail: '文件头不符合 deb 或 rpm 格式，可能不是有效的安装包。', hint: '确认文件后缀与实际格式一致，或重新下载。', retryable: false },
  PARSE_TRUNCATED: { code: 101, category: '解析', title: '文件不完整', detail: '文件被截断，读取到末尾前数据就结束了，常见于下载中断。', hint: '重新下载该安装包。', retryable: false },
  PARSE_UNSUPPORTED_COMPRESSION: { code: 102, category: '解析', title: '不支持的压缩格式', detail: '包使用了当前版本无法解压的压缩算法。', hint: '升级 RanArch，或寻找该软件的其他打包版本。', retryable: false },
  PARSE_BAD_HEADER: { code: 103, category: '解析', title: '包头损坏', detail: '包的元数据头部校验失败，文件可能已损坏。', hint: '重新下载；若多次失败，该包本身有问题。', retryable: false },

  // ---- Conflict 200 ----
  CONFLICT_PACMAN: { code: 200, category: '冲突', title: '与系统软件冲突', detail: '包内文件与 pacman 已安装的软件占用相同路径。', hint: '先卸载冲突的系统软件，或放弃本次安装。', retryable: false, showConflicts: true },
  CONFLICT_RANARCH: { code: 201, category: '冲突', title: '与已安装的转换包冲突', detail: '包内文件与之前通过 RanArch 安装的软件冲突。', hint: '可在安装记录中移除旧版本后重试。', retryable: false, showConflicts: true },
  CONFLICT_FS: { code: 202, category: '冲突', title: '文件系统冲突', detail: '目标路径被无主文件占用（不属于任何已安装软件）。', hint: '手动检查冲突路径后决定覆盖或取消。', retryable: false, showConflicts: true },

  // ---- Dep 300 ----
  DEP_UNMAPPED: { code: 300, category: '依赖', title: '依赖无法识别', detail: '部分 deb/rpm 依赖没有对应的 Arch 软件包映射。', hint: '在终端中查看缺失项，手动寻找替代包。', retryable: false, showDeps: true },
  DEP_UNRESOLVED: { code: 301, category: '依赖', title: '依赖未满足', detail: '所需依赖未安装，且未能自动解决。', hint: '允许 RanArch 调用 pacman 安装缺失依赖后重试。', retryable: true, showDeps: true },
  DEP_INSTALL_FAILED: { code: 302, category: '依赖', title: '依赖安装失败', detail: '调用 pacman 安装依赖时出错。', hint: '检查网络与镜像源（pacman -Syy），然后重试。', retryable: true, showDeps: true },

  // ---- Extract 400 ----
  EXTRACT_DISK_FULL: { code: 400, category: '解压', title: '磁盘空间不足', detail: '解压暂存区或目标分区的可用空间不够。', hint: '清理磁盘后重试。', retryable: true },
  EXTRACT_PERMISSION: { code: 401, category: '解压', title: '权限不足', detail: '写入系统目录需要管理员权限，授权被拒绝或已过期。', hint: '确认当前用户在 ranarch 组内，或通过 polkit 授权后重试。', retryable: true },
  EXTRACT_PATH_TRAVERSAL: { code: 402, category: '解压', title: '不安全的文件路径', detail: '包内包含试图跳出安装目录的路径（如 ../），已被拦截。', hint: '该包可能是恶意的，不建议继续安装。', retryable: false },

  // ---- Signature 500 ----
  SIGNATURE_BAD: { code: 500, category: '签名', title: '签名校验失败', detail: '包的数字签名与内容不匹配，文件可能被篡改。', hint: '不要安装。请从官方渠道重新获取。', retryable: false },
  SIGNATURE_MISSING: { code: 501, category: '签名', title: '包未签名', detail: '该包没有数字签名，无法验证来源。', hint: '确认来源可信后，可在选项中关闭签名校验。', retryable: false },
  SIGNATURE_KEY_MISSING: { code: 502, category: '签名', title: '缺少信任密钥', detail: '签名有效，但对应的公钥不在信任密钥环中。', hint: '导入软件发布方的公钥后重试。', retryable: true },

  // ---- Sandbox 600 ----
  SANDBOX_UNAVAILABLE: { code: 600, category: '沙箱', title: '沙箱不可用', detail: '未检测到 bubblewrap，无法进行试装预检。', hint: '安装 bubblewrap（pacman -S bubblewrap）可获得更安全的预检。', retryable: false },
  SANDBOX_TRIAL_FAILED: { code: 601, category: '沙箱', title: '试装失败', detail: '在沙箱中模拟安装时出错，真实安装也很可能失败。', hint: '查看终端日志中的具体失败点。', retryable: false },

  // ---- DB 700 ----
  DB_CORRUPT: { code: 700, category: '数据库', title: '记录数据库损坏', detail: 'RanArch 的安装记录数据库无法读取。', hint: '备份后删除 /var/lib/ranarch/ranarch.db，重启服务重建。', retryable: true },
  DB_LOCKED: { code: 701, category: '数据库', title: '数据库被占用', detail: '另一个 RanArch 进程正在使用数据库。', hint: '稍等片刻后重试。', retryable: true },

  // ---- IPC 800 ----
  IPC_AUTH: { code: 800, category: '通信', title: '服务认证失败', detail: '当前用户无权访问守护进程套接字。', hint: '把当前用户加入 ranarch 组后重新登录。', retryable: true },
  IPC_PROTOCOL: { code: 801, category: '通信', title: '通信协议错误', detail: '前后端版本不匹配，消息无法解析。', hint: '确保前端与 ranarch-daemon 为同一版本。', retryable: false },
  IPC_SESSION_NOT_FOUND: { code: 802, category: '通信', title: '会话不存在', detail: '安装会话已被清理或服务已重启。', hint: '重新发起安装。', retryable: true },

  // ---- Generic 900 ----
  INTERNAL: { code: 900, category: '系统', title: '内部错误', detail: '安装过程中发生未预期的内部错误。', hint: '重试；若持续出现请查看 /var/log/ranarch/ranarch.log。', retryable: true },
  IO: { code: 901, category: '系统', title: '读写失败', detail: '文件读写操作失败，常见于文件不存在或权限不足。', hint: '确认文件仍然存在且可读，然后重试。', retryable: true },
  CANCELLED: { code: 902, category: '系统', title: '已取消', detail: '操作被用户取消。', hint: '', retryable: true },

  // ---- 1000 ----
  BLOCKED_BY_CONFLICT: { code: 1000, category: '冲突', title: '因冲突被阻止', detail: '存在未处理的文件冲突，安装被安全策略阻止。', hint: '处理冲突项后重试。', retryable: false, showConflicts: true },
  NEEDS_USER_INPUT: { code: 1001, category: '系统', title: '需要确认', detail: '安装过程中断，等待用户决策。', hint: '在终端中查看待确认项。', retryable: true },
  AUTH_DENIED: { code: 1002, category: '权限', title: '授权被拒绝', detail: '守护进程在动系统之前会向 polkit 申请授权，这一步被取消或失败了。', hint: '重新安装并在系统认证框里输入管理员密码；若根本没有弹框，检查 polkit 是否在运行（polkit-kde-agent）以及策略是否已安装。', retryable: true },
};

/**
 * 把后端可能给出的各种写法归一到表内键名：
 *   "DEP_UNRESOLVED" → 直接命中
 *   "DepUnresolved"  → 转成 DEP_UNRESOLVED 后命中
 *   301              → 按数字码反查
 */
function toWireKey(name: string): string | null {
  if (ERROR_INFO[name]) return name;
  const upper = name.toUpperCase();
  if (ERROR_INFO[upper]) return upper;
  const snake = name
    .replace(/([a-z0-9])([A-Z])/g, '$1_$2')
    .replace(/([A-Z]+)([A-Z][a-z])/g, '$1_$2')
    .toUpperCase();
  return ERROR_INFO[snake] ? snake : null;
}

export function lookupError(code: string | number | undefined): (ErrorInfo & { name: string }) | null {
  if (code === undefined || code === null || code === '') return null;

  if (typeof code === 'string') {
    const key = toWireKey(code.trim());
    if (key) return { name: key, ...ERROR_INFO[key] };
  }

  // 按数字码反查
  const num = Number(code);
  if (!Number.isNaN(num)) {
    for (const [key, info] of Object.entries(ERROR_INFO)) {
      if (info.code === num) return { name: key, ...info };
    }
  }
  return null;
}

export function formatErrorCode(info: ErrorInfo): string {
  return 'E' + String(info.code);
}
