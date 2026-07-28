/****************************************************************************
 * 安聆 VelaGuard - 通知器实现 (PRD-06)
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "velaguard/vg_config.h"
#include "velaguard/vg_http.h"
#include "velaguard/vg_json.h"
#include "velaguard/vg_notifier.h"
#include "velaguard/vg_time.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_advice_fn g_advice_fn;
static uint32_t     g_agent_ok;
static uint32_t     g_template_fallback;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void vg_notifier_init(void)
{
  g_advice_fn = NULL;
  g_agent_ok = 0;
  g_template_fallback = 0;
}

void vg_notifier_set_advice_provider(vg_advice_fn fn)
{
  g_advice_fn = fn;
}

void vg_utf8_trim(char *s)
{
  size_t n;
  size_t i;

  if (s == NULL)
    {
      return;
    }

  n = strlen(s);
  if (n == 0)
    {
      return;
    }

  /* 从尾部回退，找到最后一个字符的起始字节并检查其长度是否完整 */

  i = n;
  while (i > 0)
    {
      unsigned char c = (unsigned char)s[i - 1];

      if ((c & 0xc0) != 0x80)
        {
          size_t need = (c < 0x80) ? 1 :
                        ((c & 0xe0) == 0xc0) ? 2 :
                        ((c & 0xf0) == 0xe0) ? 3 :
                        ((c & 0xf8) == 0xf0) ? 4 : 1;

          if (i - 1 + need > n)
            {
              s[i - 1] = '\0';
            }

          return;
        }

      i--;
    }
}

void vg_notifier_template(const vg_safety_event_t *evt,
                          char *advice, size_t len)
{
  const char *action;

  if (evt == NULL || advice == NULL || len == 0)
    {
      return;
    }

  switch (evt->type)
    {
      case VG_EVT_ALARM_BEEP:
        action = "疑似烟雾或燃气报警，建议立即电话确认，必要时联系邻居上门查看";
        break;

      case VG_EVT_WATER_FLOW:
        action = "疑似忘关水龙头或漏水，建议电话提醒关闭水阀";
        break;

      case VG_EVT_IMPACT:
        action = "疑似跌倒或物品破碎，建议立即电话确认老人是否受伤";
        break;

      case VG_EVT_DISTRESS_VOICE:
        action = "检测到疑似求助呼喊，建议立即电话确认，无人接听请尽快上门";
        break;

      case VG_EVT_NAME_CALL_HELP:
        action = "检测到急促求助呼喊，建议立即电话确认";
        break;

      default:
        action = "检测到异常声音，建议电话确认家中情况";
        break;
    }

  snprintf(advice, len, "[%s] %s。%s%s",
           vg_level_cn(evt->level), evt->summary, action,
           evt->time_reliable ? "" : "（设备时间未同步，时间仅供参考）");
  vg_utf8_trim(advice);
}

int vg_notifier_build_advice(const vg_safety_event_t *evt,
                             char *advice, size_t len)
{
  if (evt == NULL || advice == NULL || len == 0)
    {
      return -1;
    }

  advice[0] = '\0';

  if (g_advice_fn != NULL && g_advice_fn(evt, advice, len) == 0 &&
      advice[0] != '\0')
    {
      g_agent_ok++;
      return 0;
    }

  /* Agent / LLM 不可用：本地模板兜底，通知照常送达 */

  vg_notifier_template(evt, advice, len);
  g_template_fallback++;
  return 1;
}

int vg_notifier_build_payload(const vg_safety_event_t *evt,
                              const char *advice,
                              char *buf, size_t len)
{
  size_t pos;
  int n;

  if (evt == NULL || buf == NULL || len < 96)
    {
      return -1;
    }

  n = vg_event_to_json(evt, buf, len);
  if (n < 2)
    {
      return -1;
    }

  /* 把结尾的 '}' 换成 ",\"advice\":\"...\"}" */

  pos = (size_t)n - 1;
  pos += (size_t)snprintf(buf + pos, len - pos, ",\"advice\":");
  pos = vg_json_put_escaped(buf, len, pos, advice != NULL ? advice : "");

  if (pos + 2 >= len)
    {
      return -1;
    }

  buf[pos++] = '}';
  buf[pos] = '\0';
  return (int)pos;
}

int vg_notifier_webhook(const vg_safety_event_t *evt, const char *advice)
{
  vg_config_t *cfg = vg_config();
  char host[VG_URL_LEN];
  char path[VG_PATH_LEN];
  char payload[1024];
  int port = 80;
  int status = 0;

  if (!cfg->webhook_enabled || cfg->webhook_url[0] == '\0')
    {
      return -1;
    }

  if (vg_http_parse_url(cfg->webhook_url, host, sizeof(host),
                        &port, path, sizeof(path)) < 0)
    {
      return -1;
    }

  if (vg_notifier_build_payload(evt, advice, payload, sizeof(payload)) < 0)
    {
      return -1;
    }

  if (vg_http_post_json(host, port, path, payload, &status, 3000) < 0)
    {
      return -1;
    }

  return (status >= 200 && status < 300) ? 0 : -1;
}

void vg_notifier_stats(uint32_t *agent_ok, uint32_t *template_fallback)
{
  if (agent_ok != NULL)
    {
      *agent_ok = g_agent_ok;
    }

  if (template_fallback != NULL)
    {
      *template_fallback = g_template_fallback;
    }
}
