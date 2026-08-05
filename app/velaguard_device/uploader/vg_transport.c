/****************************************************************************
 * 安聆 VelaGuard - 传输适配器
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "velaguard/vg_config.h"
#include "velaguard/vg_http.h"
#include "velaguard/vg_transport.h"

#ifdef __NuttX__
#  include <nuttx/config.h>
#  ifdef CONFIG_LIB_CURL
#    include <curl/curl.h>
#  endif
#endif

#ifdef __NuttX__
#  ifdef CONFIG_LIB_CURL
struct vg_curl_response_s
{
  char  *data;
  size_t capacity;
  size_t length;
};

static bool vg_curl_header_safe(const char *value)
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

static size_t vg_curl_write(void *ptr, size_t size, size_t count,
                            void *userdata)
{
  struct vg_curl_response_s *response = userdata;
  size_t bytes = size * count;

  if (response == NULL || response->data == NULL ||
      response->length >= response->capacity ||
      bytes > response->capacity - response->length - 1)
    {
      return 0;
    }

  memcpy(response->data + response->length, ptr, bytes);
  response->length += bytes;
  response->data[response->length] = '\0';
  return bytes;
}

static int vg_transport_post_tls(const char *host, int port, const char *path,
                                 const char *server_name, const char *ca_path,
                                 const char *client_cert_path,
                                 const char *client_key_path,
                                 const char *body,
                                 const char *idempotency_key,
                                 const char *auth_token, const char *schema,
                                 char *response, size_t response_len,
                                 int *status, int timeout_ms)
{
  CURL *curl;
  CURLcode code;
  struct curl_slist *headers = NULL;
  struct curl_slist *resolve = NULL;
  struct vg_curl_response_s sink;
  char url[VG_URL_LEN + VG_PATH_LEN + 32];
  char resolve_entry[VG_URL_LEN + 32];
  char header[VG_TOKEN_LEN + VG_MESSAGE_ID_LEN + 64];
  const char *verify_host = server_name != NULL && server_name[0] != '\0' ?
                            server_name : host;
  const char *suffix = path != NULL && path[0] == '/' ? path : "/";

  if (host == NULL || host[0] == '\0' || port <= 0 || port > 65535 ||
      body == NULL || response == NULL || response_len < 2 ||
      ca_path == NULL || ca_path[0] == '\0' || verify_host == NULL ||
      verify_host[0] == '\0' || !vg_curl_header_safe(idempotency_key) ||
      !vg_curl_header_safe(auth_token) || !vg_curl_header_safe(schema))
    {
      return VG_TRANSPORT_ERR_TLS_FAILED;
    }

  response[0] = '\0';
  sink.data = response;
  sink.capacity = response_len;
  sink.length = 0;

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
    {
      return VG_TRANSPORT_ERR_TLS_FAILED;
    }

  curl = curl_easy_init();
  if (curl == NULL)
    {
      return VG_TRANSPORT_ERR_TLS_FAILED;
    }

  if (strcmp(verify_host, host) != 0)
    {
      snprintf(resolve_entry, sizeof(resolve_entry), "%s:%d:%s",
               verify_host, port, host);
      resolve = curl_slist_append(resolve, resolve_entry);
    }

  snprintf(url, sizeof(url), "https://%s:%d%s", verify_host, port, suffix);
  snprintf(header, sizeof(header), "X-VelaGuard-Schema: %s",
           schema != NULL ? schema : "");
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, header);
  headers = curl_slist_append(headers, "Expect:");
  if (idempotency_key != NULL && idempotency_key[0] != '\0')
    {
      snprintf(header, sizeof(header), "Idempotency-Key: %s", idempotency_key);
      headers = curl_slist_append(headers, header);
    }
  if (auth_token != NULL && auth_token[0] != '\0')
    {
      snprintf(header, sizeof(header), "Authorization: Bearer %s",
               auth_token);
      headers = curl_slist_append(headers, header);
    }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vg_curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
  if (client_cert_path != NULL && client_cert_path[0] != '\0')
    {
      curl_easy_setopt(curl, CURLOPT_SSLCERT, client_cert_path);
    }
  if (client_key_path != NULL && client_key_path[0] != '\0')
    {
      curl_easy_setopt(curl, CURLOPT_SSLKEY, client_key_path);
    }
  if (resolve != NULL)
    {
      curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
    }

  code = curl_easy_perform(curl);
  if (code == CURLE_OK && status != NULL)
    {
      long http_status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
      *status = (int)http_status;
    }

  curl_slist_free_all(resolve);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return code == CURLE_OK ? 0 : VG_TRANSPORT_ERR_TLS_FAILED;
}
#  endif
#endif

int vg_transport_post_json(const char *host, int port, const char *path,
                           bool secure, const char *server_name,
                           const char *ca_path, const char *client_cert_path,
                           const char *client_key_path, const char *body,
                           const char *idempotency_key,
                           const char *auth_token, const char *schema,
                           char *response, size_t response_len,
                           int *status, int timeout_ms)
{
  if (secure)
    {
#ifdef __NuttX__
#  ifdef CONFIG_LIB_CURL
      return vg_transport_post_tls(host, port, path, server_name, ca_path,
                                   client_cert_path, client_key_path, body,
                                   idempotency_key, auth_token, schema,
                                   response, response_len, status, timeout_ms);
#  else
      return VG_TRANSPORT_ERR_TLS_UNAVAILABLE;
#  endif
#else
      (void)server_name;
      (void)ca_path;
      (void)client_cert_path;
      (void)client_key_path;
      return VG_TRANSPORT_ERR_TLS_UNAVAILABLE;
#endif
    }

  return vg_http_post_json_ex(host, port, path, body, idempotency_key,
                              auth_token, schema, response, response_len,
                              status, timeout_ms);
}
