// SPDX-License-Identifier: MIT
// RanArch RD Installer — 特权操作的授权检查（polkit）。
//
// 守护进程以 root 运行，任何会改动系统的请求（安装 / 卸载 / 管理信任密钥）都必须
// 先向 polkit 申请授权，由桌面环境弹出认证框；授权对象是**发起该请求的客户端进程**，
// 而不是守护进程自己。策略见 polkit/org.ranarch.daemon.policy
// （action 的 defaults 为 auth_admin_keep）。
//
// 安全约定：编译未启用 polkit、或 polkit 不可用时，一律**拒绝**，绝不静默放行
// （root 调用者除外 —— polkit 对其也会直接放行）。
#pragma once

#include <sys/types.h>

#include <string>

namespace ranarch {

/** polkit 动作 id（与 polkit/org.ranarch.daemon.policy 中的 id 一一对应） */
inline constexpr const char* kActionInstall = "org.ranarch.daemon.install";
inline constexpr const char* kActionRemove  = "org.ranarch.daemon.remove";
inline constexpr const char* kActionKeys    = "org.ranarch.daemon.manage-keys";

/**
 * 为调用者（peer_pid / peer_uid）申请执行 `action_id` 的授权。
 *
 * 同步阻塞：必要时会一直等到用户在认证框上确认或取消 —— 调用方是单线程事件循环，
 * 与「安装本身也在事件循环里同步跑」的既有设计一致。
 *
 * @param[out] err 失败原因（可直接回给客户端）
 * @return true = 已授权，可以继续；false = 拒绝 / 无法请求授权
 */
bool request_authorization(pid_t peer_pid, uid_t peer_uid,
                           const char* action_id, std::string& err);

} // namespace ranarch
