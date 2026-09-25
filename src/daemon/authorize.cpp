// SPDX-License-Identifier: GPL-3.0-or-later
// RanArch RD Installer — 授权检查实现（polkit）。
#include "authorize.h"

#include "core/logger.h"

#include <unistd.h>

#ifdef RANARCH_HAVE_POLKIT
#include <polkit/polkit.h>
#endif

namespace ranarch {

bool request_authorization(pid_t peer_pid, uid_t peer_uid,
                           const char* action_id, std::string& err) {
    // 只有守护进程真的具备特权（以 root 运行）时才需要请求授权：
    // 非特权实例（开发时以当前用户前台跑）本来也写不了系统目录，
    // 再弹一次密码框只会干扰调试。
    if (::geteuid() != 0) {
        RA_LOG_WARN("authorization skipped (daemon is not root): action=%s pid=%d uid=%u",
                    action_id, static_cast<int>(peer_pid), static_cast<unsigned>(peer_uid));
        return true;
    }

    // root 调用者由 polkit 直接放行，这里省掉一次往返。
    if (peer_uid == 0) return true;

    if (peer_pid <= 0) {
        err = "无法确定调用方进程，拒绝该操作";
        RA_LOG_ERROR("authorization denied: cannot determine peer pid (action=%s)", action_id);
        return false;
    }

#ifndef RANARCH_HAVE_POLKIT
    err = "本版本未编译 polkit 支持，出于安全考虑拒绝该操作";
    RA_LOG_ERROR("authorization denied (built without polkit): action=%s pid=%d uid=%u",
                 action_id, static_cast<int>(peer_pid), static_cast<unsigned>(peer_uid));
    return false;
#else
    GError* gerr = nullptr;

    PolkitAuthority* authority = polkit_authority_get_sync(nullptr, &gerr);
    if (!authority) {
        err = std::string("无法连接 polkit：") + (gerr ? gerr->message : "unknown");
        if (gerr) g_error_free(gerr);
        RA_LOG_ERROR("polkit authority unavailable: %s", err.c_str());
        return false;
    }

    // 用发起请求的客户端进程作为 subject —— 弹框时用户看到的是「谁在请求授权」。
    PolkitSubject* subject = polkit_unix_process_new_for_owner(peer_pid, 0,
                                                              static_cast<gint>(peer_uid));
    PolkitAuthorizationResult* result = polkit_authority_check_authorization_sync(
        authority, subject, action_id, nullptr,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
        nullptr, &gerr);

    bool allowed = false;
    if (!result) {
        err = std::string("授权检查失败：") + (gerr ? gerr->message : "unknown");
        RA_LOG_ERROR("polkit check_authorization failed: %s", err.c_str());
    } else {
        allowed = polkit_authorization_result_get_is_authorized(result) != 0;
        if (allowed) {
            RA_LOG_INFO("authorized: action=%s pid=%d uid=%u",
                        action_id, static_cast<int>(peer_pid), static_cast<unsigned>(peer_uid));
        } else {
            err = "授权被拒绝：需要管理员认证";
            RA_LOG_WARN("authorization denied by polkit: action=%s pid=%d uid=%u",
                        action_id, static_cast<int>(peer_pid), static_cast<unsigned>(peer_uid));
        }
        g_object_unref(result);
    }

    if (gerr) g_error_free(gerr);
    g_object_unref(subject);
    g_object_unref(authority);
    return allowed;
#endif
}

} // namespace ranarch
