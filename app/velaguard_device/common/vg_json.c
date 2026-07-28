/****************************************************************************
 * 安聆 VelaGuard - 极简 JSON 读写辅助实现
 ****************************************************************************/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "velaguard/vg_json.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 在 json 中定位 "key" 之后的值起始位置，跳过空白与冒号。
 * 只在对象键位置匹配（即引号包裹且紧跟冒号），避免命中普通字符串内容。
 */

static const char *vg_json_find_value(const char *json, const char *key)
{
  size_t keylen = strlen(key);
  const char *p = json;

  if (json == NULL || key == NULL)
    {
      return NULL;
    }

  while ((p = strchr(p, '"')) != NULL)
    {
      const char *k = p + 1;

      if (strncmp(k, key, keylen) == 0 && k[keylen] == '"')
        {
          const char *q = k + keylen + 1;

          while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
            {
              q++;
            }

          if (*q == ':')
            {
              q++;
              while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
                {
                  q++;
                }

              return q;
            }
        }

      p = k;
    }

  return NULL;
}

/* 把 UTF-16 码点写成 UTF-8，返回写入字节数 */

static size_t vg_utf8_encode(unsigned int cp, char *out, size_t room)
{
  if (cp < 0x80 && room >= 1)
    {
      out[0] = (char)cp;
      return 1;
    }

  if (cp < 0x800 && room >= 2)
    {
      out[0] = (char)(0xc0 | (cp >> 6));
      out[1] = (char)(0x80 | (cp & 0x3f));
      return 2;
    }

  if (room >= 3)
    {
      out[0] = (char)(0xe0 | (cp >> 12));
      out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
      out[2] = (char)(0x80 | (cp & 0x3f));
      return 3;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_json_get_str(const char *json, const char *key, char *out, size_t len)
{
  const char *v = vg_json_find_value(json, key);
  size_t o = 0;

  if (v == NULL || *v != '"' || out == NULL || len == 0)
    {
      return -1;
    }

  v++;
  while (*v != '\0' && *v != '"' && o + 1 < len)
    {
      if (*v == '\\')
        {
          v++;
          switch (*v)
            {
              case 'n': out[o++] = '\n'; v++; break;
              case 't': out[o++] = '\t'; v++; break;
              case 'r': out[o++] = '\r'; v++; break;
              case 'b': out[o++] = '\b'; v++; break;
              case 'f': out[o++] = '\f'; v++; break;
              case 'u':
                {
                  char hex[5];
                  unsigned int cp;
                  size_t n;

                  if (strlen(v + 1) < 4)
                    {
                      v += strlen(v);
                      break;
                    }

                  memcpy(hex, v + 1, 4);
                  hex[4] = '\0';
                  cp = (unsigned int)strtoul(hex, NULL, 16);
                  n = vg_utf8_encode(cp, out + o, len - 1 - o);
                  o += n;
                  v += 5;
                }
                break;
              case '\0':
                break;
              default:
                out[o++] = *v++;
                break;
            }
        }
      else
        {
          out[o++] = *v++;
        }
    }

  out[o] = '\0';
  return 0;
}

int vg_json_get_double(const char *json, const char *key, double *out)
{
  const char *v = vg_json_find_value(json, key);

  if (v == NULL || out == NULL)
    {
      return -1;
    }

  if (*v != '-' && *v != '+' && !isdigit((unsigned char)*v))
    {
      return -1;
    }

  *out = strtod(v, NULL);
  return 0;
}

int vg_json_get_int(const char *json, const char *key, int *out)
{
  double d;

  if (vg_json_get_double(json, key, &d) < 0)
    {
      return -1;
    }

  *out = (int)d;
  return 0;
}

int vg_json_get_i64(const char *json, const char *key, long long *out)
{
  const char *v = vg_json_find_value(json, key);

  if (v == NULL || out == NULL)
    {
      return -1;
    }

  if (*v != '-' && *v != '+' && !isdigit((unsigned char)*v))
    {
      return -1;
    }

  *out = strtoll(v, NULL, 10);
  return 0;
}

int vg_json_get_bool(const char *json, const char *key, bool *out)
{
  const char *v = vg_json_find_value(json, key);

  if (v == NULL || out == NULL)
    {
      return -1;
    }

  if (strncmp(v, "true", 4) == 0)
    {
      *out = true;
      return 0;
    }

  if (strncmp(v, "false", 5) == 0)
    {
      *out = false;
      return 0;
    }

  return -1;
}

size_t vg_json_put_escaped(char *buf, size_t len, size_t pos, const char *s)
{
  if (buf == NULL || pos + 1 >= len)
    {
      return pos;
    }

  buf[pos++] = '"';

  for (; s != NULL && *s != '\0'; s++)
    {
      unsigned char c = (unsigned char)*s;
      const char *esc = NULL;

      switch (c)
        {
          case '"':  esc = "\\\""; break;
          case '\\': esc = "\\\\"; break;
          case '\n': esc = "\\n";  break;
          case '\r': esc = "\\r";  break;
          case '\t': esc = "\\t";  break;
          default:   break;
        }

      if (esc != NULL)
        {
          if (pos + 2 >= len)
            {
              break;
            }

          buf[pos++] = esc[0];
          buf[pos++] = esc[1];
        }
      else if (c < 0x20)
        {
          if (pos + 6 >= len)
            {
              break;
            }

          snprintf(buf + pos, len - pos, "\\u%04x", c);
          pos += 6;
        }
      else
        {
          if (pos + 1 >= len)
            {
              break;
            }

          buf[pos++] = (char)c;
        }
    }

  if (pos + 1 < len)
    {
      buf[pos++] = '"';
    }

  buf[pos] = '\0';
  return pos;
}
