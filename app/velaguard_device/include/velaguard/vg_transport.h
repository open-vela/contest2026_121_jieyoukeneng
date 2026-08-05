/****************************************************************************
 * 安聆 VelaGuard - 传输抽象边界
 *
 * 业务上传器只依赖这个接口。当前主机/演示构建提供明文 HTTP 适配器；
 * 生产 profile 在 TLS 适配器接入前必须被拒绝，不能把明文实现当作安全通道。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_TRANSPORT_H
#define __VELAGUARD_VG_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define VG_TRANSPORT_ERR_TLS_UNAVAILABLE (-120)

int vg_transport_post_json(const char *host, int port, const char *path,
                           bool secure, const char *server_name,
                           const char *ca_path, const char *body,
                           const char *idempotency_key,
                           const char *auth_token, const char *schema,
                           char *response, size_t response_len,
                           int *status, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_TRANSPORT_H */
