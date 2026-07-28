/****************************************************************************
 * 安聆 VelaGuard - 事件上传与断网补发实现 (PRD-05)
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "velaguard/vg_config.h"
#include "velaguard/vg_http.h"
#include "velaguard/vg_notifier.h"
#include "velaguard/vg_time.h"
#include "velaguard/vg_uploader.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_UPLOAD_QUEUE_MAX 32
#define VG_PAYLOAD_MAX      1024

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct
{
  bool     used;
  char     event_id[VG_EVENT_ID_LEN];
  char     payload[VG_PAYLOAD_MAX];
  uint64_t next_try_ms;
  uint32_t attempts;
} vg_upload_item_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_upload_item_t g_queue[VG_UPLOAD_QUEUE_MAX];
static char             g_pending_path[VG_PATH_LEN + 32];
static bool             g_online;
static uint32_t         g_sent;
static uint32_t         g_failed;
static bool             g_inited;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 待发送队列落盘，断电重启后仍能补发（每行一条 payload） */

static void vg_pending_save(void)
{
  FILE *fp;
  int i;

  if (g_pending_path[0] == '\0')
    {
      return;
    }

  fp = fopen(g_pending_path, "w");
  if (fp == NULL)
    {
      return;
    }

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (g_queue[i].used)
        {
          fprintf(fp, "%s\n", g_queue[i].payload);
        }
    }

  fflush(fp);
  fsync(fileno(fp));
  fclose(fp);
}

static void vg_pending_load(void)
{
  char line[VG_PAYLOAD_MAX];
  FILE *fp;
  int i = 0;

  if (g_pending_path[0] == '\0')
    {
      return;
    }

  fp = fopen(g_pending_path, "r");
  if (fp == NULL)
    {
      return;
    }

  while (i < VG_UPLOAD_QUEUE_MAX && fgets(line, sizeof(line), fp) != NULL)
    {
      vg_safety_event_t evt;
      size_t len = strlen(line);

      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
          line[--len] = '\0';
        }

      if (len == 0 || vg_event_from_json(line, &evt) < 0)
        {
          continue;
        }

      g_queue[i].used = true;
      vg_strlcpy(g_queue[i].event_id, evt.event_id, sizeof(g_queue[i].event_id));
      vg_strlcpy(g_queue[i].payload, line, sizeof(g_queue[i].payload));
      g_queue[i].next_try_ms = 0;
      g_queue[i].attempts = 0;
      i++;
    }

  fclose(fp);

  if (i > 0)
    {
      printf("[velaguard] 从断点恢复 %d 条待发送通知\n", i);
    }
}

static int vg_try_send(vg_upload_item_t *item)
{
  vg_config_t *cfg = vg_config();
  int status = 0;
  int ret;

  ret = vg_http_post_json(cfg->console_host, cfg->console_port,
                          cfg->console_path, item->payload, &status, 3000);

  if (ret == 0 && status >= 200 && status < 300)
    {
      return 0;
    }

  return -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_uploader_init(void)
{
  vg_config_t *cfg = vg_config();

  memset(g_queue, 0, sizeof(g_queue));
  snprintf(g_pending_path, sizeof(g_pending_path),
           "%s/pending.jsonl", cfg->data_dir);

  g_online = false;
  g_sent = 0;
  g_failed = 0;
  g_inited = true;

  vg_pending_load();
  return 0;
}

void vg_uploader_deinit(void)
{
  vg_pending_save();
  g_inited = false;
}

int vg_uploader_enqueue(const vg_safety_event_t *evt,
                        vg_upload_reason_t reason)
{
  return vg_uploader_enqueue_advice(evt, reason, NULL);
}

int vg_uploader_enqueue_advice(const vg_safety_event_t *evt,
                               vg_upload_reason_t reason,
                               const char *advice_in)
{
  vg_safety_event_t local;
  char advice[VG_ADVICE_LEN];
  vg_upload_item_t *slot = NULL;
  int i;

  if (!g_inited || evt == NULL)
    {
      return -1;
    }

  local = *evt;
  local.upload_reason = reason;

  /* 通知文案：调用方（ai_agent Skill 回灌）给了就直接用，
   * 否则走 Agent 规则表 -> 内置模板的自动回退。
   */

  if (advice_in != NULL && advice_in[0] != '\0')
    {
      vg_strlcpy(advice, advice_in, sizeof(advice));
      vg_utf8_trim(advice);
    }
  else
    {
      vg_notifier_build_advice(&local, advice, sizeof(advice));
    }

  /* 幂等：同一 eventId 覆盖为最新状态 */

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (g_queue[i].used &&
          strcmp(g_queue[i].event_id, local.event_id) == 0)
        {
          slot = &g_queue[i];
          break;
        }
    }

  if (slot == NULL)
    {
      for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
        {
          if (!g_queue[i].used)
            {
              slot = &g_queue[i];
              break;
            }
        }
    }

  if (slot == NULL)
    {
      /* 队列写满：丢弃最旧一条，保证最新高危事件能发出 */

      slot = &g_queue[0];
    }

  memset(slot, 0, sizeof(*slot));
  slot->used = true;
  vg_strlcpy(slot->event_id, local.event_id, sizeof(slot->event_id));

  if (vg_notifier_build_payload(&local, advice, slot->payload,
                                sizeof(slot->payload)) < 0)
    {
      slot->used = false;
      return -1;
    }

  slot->next_try_ms = 0;
  slot->attempts = 0;

  vg_pending_save();

  /* 可选扩展通道：失败不影响主通道 */

  vg_notifier_webhook(&local, advice);

  vg_uploader_tick();
  return 0;
}

void vg_uploader_tick(void)
{
  vg_config_t *cfg = vg_config();
  uint64_t now = vg_now_ms();
  bool changed = false;
  int i;

  if (!g_inited)
    {
      return;
    }

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      vg_upload_item_t *item = &g_queue[i];
      uint32_t backoff;

      if (!item->used || now < item->next_try_ms)
        {
          continue;
        }

      if (vg_try_send(item) == 0)
        {
          item->used = false;
          g_sent++;
          g_online = true;
          changed = true;
          continue;
        }

      item->attempts++;
      g_failed++;
      g_online = false;

      /* 线性退避，封顶 6 倍，保证恢复后能快速补发 */

      backoff = cfg->upload_retry_sec *
                (item->attempts < 6 ? item->attempts : 6);
      item->next_try_ms = now + (uint64_t)backoff * 1000;
    }

  if (changed)
    {
      vg_pending_save();
    }
}

int vg_uploader_pending(void)
{
  int n = 0;
  int i;

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (g_queue[i].used)
        {
          n++;
        }
    }

  return n;
}

bool vg_uploader_online(void)
{
  return g_online;
}

void vg_uploader_stats(uint32_t *sent, uint32_t *failed)
{
  if (sent != NULL)
    {
      *sent = g_sent;
    }

  if (failed != NULL)
    {
      *failed = g_failed;
    }
}

void vg_uploader_flush(void)
{
  int i;

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      g_queue[i].next_try_ms = 0;
    }

  vg_uploader_tick();
}
