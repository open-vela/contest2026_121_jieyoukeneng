/****************************************************************************
 * 安聆 VelaGuard - 传输抽象边界
 *
 * 业务上传器只依赖这个接口。演示 profile 使用明文 HTTP；Gemini-S1
 * 的生产 profile 使用板端 libcurl + mbedTLS，主机线明确拒绝 TLS，
 * 避免测试程序把明文实现冒充为安全通道。
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
#define VG_TRANSPORT_ERR_TLS_FAILED      (-121)

int vg_transport_post_json(const char *host, int port, const char *path,
                           bool secure, const char *server_name,
                           const char *ca_path, const char *client_cert_path,
                           const char *client_key_path, const char *body,
                           const char *idempotency_key,
                           const char *auth_token, const char *schema,
                           char *response, size_t response_len,
                           int *status, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_TRANSPORT_H */
