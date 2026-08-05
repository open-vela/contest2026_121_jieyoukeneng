/****************************************************************************
 * 安聆 VelaGuard - 明文 HTTP 演示传输实现
 *
 * 该文件只保留 demo profile 的最小 IPv4/HTTP 联调能力。生产 profile 不得
 * 调用它，生产通道应由同一 vg_http_post_json_ex 契约替换为官方 TLS/mTLS
 * socket 实现。
 ****************************************************************************/

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "velaguard/vg_http.h"
#include "velaguard/vg_types.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_HTTP_RESP_MAX 4096

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool vg_http_header_value_safe(const char *value)
{
  size_t i;

  if (value == NULL)
    {
      return true;
    }

  for (i = 0; value[i] != '\0'; i++)
    {
      if (value[i] == '\r' || value[i] == '\n')
        {
          return false;
        }
    }

  return true;
}

static int vg_http_send_all(int sock, const char *buf, size_t len)
{
  size_t sent = 0;

  while (sent < len)
    {
      ssize_t n = send(sock, buf + sent, len - sent, 0);

      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          return -1;
        }

      sent += (size_t)n;
    }

  return 0;
}

static int vg_http_append_header(char *buf, size_t len, size_t *used,
                                 const char *name, const char *value)
{
  int n;

  if (buf == NULL || used == NULL || name == NULL || value == NULL ||
      value[0] == '\0' || *used >= len)
    {
      return 0;
    }

  n = snprintf(buf + *used, len - *used, "%s%s\r\n", name, value);
  if (n < 0 || (size_t)n >= len - *used)
    {
      return -1;
    }

  *used += (size_t)n;
  return 0;
}

static int vg_http_parse_status(char *response, int *status,
                                char **body)
{
  char *separator;
  int code;

  separator = strstr(response, "\r\n\r\n");
  if (separator == NULL ||
      sscanf(response, "HTTP/1.%*d %d", &code) != 1)
    {
      return -1;
    }

  if (status != NULL)
    {
      *status = code;
    }

  if (body != NULL)
    {
      *body = separator + 4;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_http_parse_url_ex(const char *url, char *host, size_t hostlen,
                         int *port, char *path, size_t pathlen,
                         bool *secure)
{
  const char *p;
  const char *scheme_end;
  const char *colon;
  const char *slash;
  size_t n;
  bool is_secure = false;

  if (url == NULL || host == NULL || hostlen == 0 || port == NULL ||
      path == NULL || pathlen == 0)
    {
      return -1;
    }

  scheme_end = strstr(url, "://");
  if (scheme_end != NULL)
    {
      size_t scheme_len = (size_t)(scheme_end - url);

      if (scheme_len == 5 && strncmp(url, "https", 5) == 0)
        {
          is_secure = true;
        }
      else if (scheme_len != 4 || strncmp(url, "http", 4) != 0)
        {
          return -1;
        }

      p = scheme_end + 3;
    }
  else
    {
      p = url;
    }

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

  if (colon != NULL)
    {
      char *end;
      long parsed;

      errno = 0;
      parsed = strtol(colon + 1, &end, 10);
      if (errno == ERANGE || end == colon + 1 ||
          (slash == NULL ? *end != '\0' : end != slash) ||
          parsed <= 0 || parsed > 65535)
        {
          return -1;
        }

      *port = (int)parsed;
    }
  else
    {
      *port = is_secure ? 443 : 80;
    }

  if (slash != NULL)
    {
      if (strlen(slash) >= pathlen)
        {
          return -1;
        }

      vg_strlcpy(path, slash, pathlen);
    }
  else
    {
      vg_strlcpy(path, "/", pathlen);
    }

  if (secure != NULL)
    {
      *secure = is_secure;
    }

  return 0;
}

int vg_http_parse_url(const char *url, char *host, size_t hostlen,
                      int *port, char *path, size_t pathlen)
{
  return vg_http_parse_url_ex(url, host, hostlen, port, path, pathlen, NULL);
}

int vg_http_post_json_ex(const char *host, int port, const char *path,
                         const char *body, const char *idempotency_key,
                         const char *auth_token, const char *schema,
                         char *response, size_t response_len,
                         int *status, int timeout_ms)
{
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  struct addrinfo *addr;
  struct timeval tv;
  char service[8];
  char header[1024];
  char raw_response[VG_HTTP_RESP_MAX];
  char *response_body;
  size_t body_len;
  size_t header_len;
  size_t used = 0;
  int sock = -1;
  int ret = -1;

  if (status != NULL)
    {
      *status = 0;
    }
  if (response != NULL && response_len > 0)
    {
      response[0] = '\0';
    }

  if (host == NULL || path == NULL || body == NULL || port <= 0 ||
      port > 65535 || timeout_ms <= 0 ||
      !vg_http_header_value_safe(host) ||
      !vg_http_header_value_safe(path) ||
      !vg_http_header_value_safe(idempotency_key) ||
      !vg_http_header_value_safe(auth_token) ||
      !vg_http_header_value_safe(schema))
    {
      return -1;
    }

  body_len = strlen(body);
  if (body_len > 64 * 1024)
    {
      return -1;
    }

  snprintf(service, sizeof(service), "%d", port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, service, &hints, &addresses) != 0)
    {
      return -2;
    }

  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  for (addr = addresses; addr != NULL; addr = addr->ai_next)
    {
      sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
      if (sock < 0)
        {
          continue;
        }

      (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      if (connect(sock, addr->ai_addr, addr->ai_addrlen) == 0)
        {
          break;
        }

      close(sock);
      sock = -1;
    }

  if (sock < 0)
    {
      freeaddrinfo(addresses);
      return -3;
    }

  {
    int n = snprintf(header, sizeof(header),
                     "POST %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "Content-Type: application/json\r\n"
                     "Accept: application/json\r\n"
                     "Connection: close\r\n"
                     "Content-Length: %zu\r\n",
                     path, host, port, body_len);

    if (n < 0 || (size_t)n >= sizeof(header))
      {
        close(sock);
        freeaddrinfo(addresses);
        return -4;
      }

    header_len = (size_t)n;
    if (vg_http_append_header(header, sizeof(header), &header_len,
                              "X-VelaGuard-Schema: ", schema) < 0 ||
        vg_http_append_header(header, sizeof(header), &header_len,
                              "Idempotency-Key: ", idempotency_key) < 0 ||
        vg_http_append_header(header, sizeof(header), &header_len,
                              "Authorization: Bearer ", auth_token) < 0 ||
        header_len + 2 >= sizeof(header))
      {
        close(sock);
        freeaddrinfo(addresses);
        return -4;
      }

    header[header_len++] = '\r';
    header[header_len++] = '\n';
    if (vg_http_send_all(sock, header, header_len) < 0 ||
        vg_http_send_all(sock, body, body_len) < 0)
      {
        close(sock);
        freeaddrinfo(addresses);
        return -4;
      }
  }

  while (used + 1 < sizeof(raw_response))
    {
      ssize_t n = recv(sock, raw_response + used,
                       sizeof(raw_response) - used - 1, 0);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }
      if (n < 0)
        {
          close(sock);
          freeaddrinfo(addresses);
          return -5;
        }
      if (n == 0)
        {
          break;
        }

      used += (size_t)n;
    }

  raw_response[used] = '\0';
  if (used + 1 >= sizeof(raw_response) ||
      vg_http_parse_status(raw_response, status, &response_body) < 0)
    {
      close(sock);
      freeaddrinfo(addresses);
      return -6;
    }

  if (response != NULL && response_len > 0)
    {
      size_t response_size = strlen(response_body);
      if (response_size >= response_len)
        {
          close(sock);
          freeaddrinfo(addresses);
          return -7;
        }

      memcpy(response, response_body, response_size + 1);
    }

  ret = 0;
  close(sock);
  freeaddrinfo(addresses);
  return ret;
}

int vg_http_post_json(const char *host, int port, const char *path,
                      const char *body, int *status, int timeout_ms)
{
  return vg_http_post_json_ex(host, port, path, body, NULL, NULL, NULL,
                              NULL, 0, status, timeout_ms);
}
