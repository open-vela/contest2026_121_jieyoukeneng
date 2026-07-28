/****************************************************************************
 * 安聆 VelaGuard - 极简 HTTP 客户端 (PRD-05 上传通道)
 *
 * 只用于向局域网控制台 POST 结构化事件摘要，不承载任何原始音频。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_HTTP_H
#define __VELAGUARD_VG_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 发送一次 HTTP/1.1 POST，body 为 JSON。
 *
 * 返回 0 表示 TCP 与 HTTP 交互完成（status 输出 HTTP 状态码）；
 * 返回负值表示网络层失败（断网、超时、连接被拒）。
 */

int vg_http_post_json(const char *host, int port, const char *path,
                      const char *body, int *status, int timeout_ms);

/* 解析形如 http://192.168.43.1:8080/hook 的 URL。成功返回 0。 */

int vg_http_parse_url(const char *url, char *host, size_t hostlen,
                      int *port, char *path, size_t pathlen);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_HTTP_H */
