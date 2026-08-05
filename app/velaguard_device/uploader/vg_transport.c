/****************************************************************************
 * 安聆 VelaGuard - 传输适配器
 ****************************************************************************/

#include "velaguard/vg_http.h"
#include "velaguard/vg_transport.h"

int vg_transport_post_json(const char *host, int port, const char *path,
                           bool secure, const char *server_name,
                           const char *ca_path, const char *body,
                           const char *idempotency_key,
                           const char *auth_token, const char *schema,
                           char *response, size_t response_len,
                           int *status, int timeout_ms)
{
  (void)server_name;
  (void)ca_path;

  /* 这里是明确的安全门：没有 TLS 实现时，绝不静默回退到明文。 */

  if (secure)
    {
      return VG_TRANSPORT_ERR_TLS_UNAVAILABLE;
    }

  return vg_http_post_json_ex(host, port, path, body, idempotency_key,
                              auth_token, schema, response, response_len,
                              status, timeout_ms);
}
