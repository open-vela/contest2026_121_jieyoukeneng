/****************************************************************************
 * 安聆 VelaGuard - 极简 HTTP 客户端实现
 ****************************************************************************/

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "velaguard/vg_types.h"
#include "velaguard/vg_http.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_HTTP_RESP_MAX 512

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_http_parse_url(const char *url, char *host, size_t hostlen,
                      int *port, char *path, size_t pathlen)
{
  const char *p;
  const char *colon;
  const char *slash;
  size_t n;

  if (url == NULL || host == NULL || port == NULL || path == NULL)
    {
      return -1;
    }

  p = strstr(url, "://");
  p = (p != NULL) ? p + 3 : url;

  slash = strchr(p, '/');
  colon = strchr(p, ':');

  if (slash != NULL && colon != NULL && colon > slash)
    {
      colon = NULL;
    }

  n = (colon != NULL) ? (size_t)(colon - p)
                      : (slash != NULL ? (size_t)(slash - p) : strlen(p));

  if (n == 0 || n >= hostlen)
    {
      return -1;
    }

  memcpy(host, p, n);
  host[n] = '\0';

  *port = (colon != NULL) ? atoi(colon + 1) : 80;
  if (*port <= 0 || *port > 65535)
    {
      *port = 80;
    }

  if (slash != NULL)
    {
      vg_strlcpy(path, slash, pathlen);
      path[pathlen - 1] = '\0';
    }
  else
    {
      vg_strlcpy(path, "/", pathlen);
      path[pathlen - 1] = '\0';
    }

  return 0;
}

int vg_http_post_json(const char *host, int port, const char *path,
                      const char *body, int *status, int timeout_ms)
{
  struct sockaddr_in addr;
  struct timeval tv;
  char header[320];
  char resp[VG_HTTP_RESP_MAX];
  size_t body_len;
  ssize_t n;
  int sock;
  int ret = -1;

  if (host == NULL || path == NULL || body == NULL)
    {
      return -1;
    }

  body_len = strlen(body);

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);

  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
      /* 演示组网使用手机热点 IP，无需 DNS；域名场景请在配置中直接写 IP */

      return -2;
    }

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      return -3;
    }

  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
      close(sock);
      return -4;
    }

  snprintf(header, sizeof(header),
           "POST %s HTTP/1.1\r\n"
           "Host: %s:%d\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: %zu\r\n"
           "Connection: close\r\n\r\n",
           path, host, port, body_len);

  if (send(sock, header, strlen(header), 0) < 0 ||
      send(sock, body, body_len, 0) < 0)
    {
      close(sock);
      return -5;
    }

  n = recv(sock, resp, sizeof(resp) - 1, 0);
  if (n > 0)
    {
      resp[n] = '\0';
      if (status != NULL)
        {
          int code = 0;

          if (sscanf(resp, "HTTP/1.%*d %d", &code) == 1)
            {
              *status = code;
            }
          else
            {
              *status = 0;
            }
        }

      ret = 0;
    }
  else
    {
      ret = -6;
    }

  close(sock);
  return ret;
}
