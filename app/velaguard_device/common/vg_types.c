/****************************************************************************
 * 安聆 VelaGuard - 公共类型与结构化事件协议实现 (PRD-05)
 ****************************************************************************/

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "velaguard/vg_types.h"
#include "velaguard/vg_json.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_event_type_str[VG_EVT_TYPE_MAX] =
{
  "alarm_beep", "distress_voice", "water_flow", "name_call_help", "impact"
};

static const char * const g_event_type_cn[VG_EVT_TYPE_MAX] =
{
  "烟雾/燃气报警", "呻吟或呼救", "持续水流", "呼喊姓名/求救词", "破碎或撞击"
};

static const char * const g_level_str[] =
{
  "notice", "warning", "emergency"
};

static const char * const g_level_cn[] =
{
  "提醒", "警告", "紧急"
};

static const char * const g_status_str[] =
{
  "pending", "handled", "false_alarm", "snoozed", "no_response"
};

static const char * const g_status_cn[] =
{
  "待处理", "已处理", "误报", "稍后提醒", "无人响应"
};

static const char * const g_upload_reason_str[] =
{
  "manual_test", "escalated", "sync"
};

static const char * const g_state_str[] =
{
  "idle", "suspect", "notice", "warning", "emergency", "snoozed", "closed"
};

static const char * const g_sound_class_str[VG_SOUND_CLASS_MAX] =
{
  "other", "alarm_beep", "water_flow", "impact"
};

static const char * const g_voice_kind_str[VG_VOICE_KIND_MAX] =
{
  "none", "moan", "scream", "shout_help", "name_call", "help_phrase"
};

/* PRD-03 并发仲裁：同级别按类别优先级
 * distress_voice >= name_call_help > alarm_beep > impact > water_flow
 */

static const int g_type_priority[VG_EVT_TYPE_MAX] =
{
  [VG_EVT_DISTRESS_VOICE]  = 5,
  [VG_EVT_NAME_CALL_HELP]  = 4,
  [VG_EVT_ALARM_BEEP]      = 3,
  [VG_EVT_IMPACT]          = 2,
  [VG_EVT_WATER_FLOW]      = 1,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void vg_strlcpy(char *dst, const char *src, size_t size)
{
  size_t n;

  if (dst == NULL || size == 0)
    {
      return;
    }

  if (src == NULL)
    {
      dst[0] = '\0';
      return;
    }

  n = strlen(src);
  if (n >= size)
    {
      n = size - 1;
    }

  memcpy(dst, src, n);
  dst[n] = '\0';
}

const char *vg_event_type_str(vg_event_type_t type)
{
  if (type < 0 || type >= VG_EVT_TYPE_MAX)
    {
      return "unknown";
    }

  return g_event_type_str[type];
}

const char *vg_event_type_cn(vg_event_type_t type)
{
  if (type < 0 || type >= VG_EVT_TYPE_MAX)
    {
      return "未知事件";
    }

  return g_event_type_cn[type];
}

const char *vg_level_str(vg_level_t level)
{
  if (level < VG_LEVEL_NOTICE || level > VG_LEVEL_EMERGENCY)
    {
      return "none";
    }

  return g_level_str[level];
}

const char *vg_level_cn(vg_level_t level)
{
  if (level < VG_LEVEL_NOTICE || level > VG_LEVEL_EMERGENCY)
    {
      return "无";
    }

  return g_level_cn[level];
}

const char *vg_status_str(vg_local_status_t status)
{
  if (status < 0 || status > VG_STATUS_NO_RESPONSE)
    {
      return "pending";
    }

  return g_status_str[status];
}

const char *vg_status_cn(vg_local_status_t status)
{
  if (status < 0 || status > VG_STATUS_NO_RESPONSE)
    {
      return "待处理";
    }

  return g_status_cn[status];
}

const char *vg_upload_reason_str(vg_upload_reason_t reason)
{
  if (reason < 0 || reason > VG_UPLOAD_SYNC)
    {
      return "sync";
    }

  return g_upload_reason_str[reason];
}

const char *vg_state_str(vg_state_t state)
{
  if (state < 0 || state > VG_STATE_CLOSED)
    {
      return "idle";
    }

  return g_state_str[state];
}

const char *vg_sound_class_str(vg_sound_class_t cls)
{
  if (cls < 0 || cls >= VG_SOUND_CLASS_MAX)
    {
      return "other";
    }

  return g_sound_class_str[cls];
}

const char *vg_voice_kind_str(vg_voice_kind_t kind)
{
  if (kind < 0 || kind >= VG_VOICE_KIND_MAX)
    {
      return "none";
    }

  return g_voice_kind_str[kind];
}

int vg_event_type_parse(const char *s, vg_event_type_t *out)
{
  int i;

  if (s == NULL || out == NULL)
    {
      return -1;
    }

  for (i = 0; i < VG_EVT_TYPE_MAX; i++)
    {
      if (strcmp(s, g_event_type_str[i]) == 0)
        {
          *out = (vg_event_type_t)i;
          return 0;
        }
    }

  return -1;
}

int vg_level_parse(const char *s, vg_level_t *out)
{
  int i;

  if (s == NULL || out == NULL)
    {
      return -1;
    }

  for (i = 0; i <= VG_LEVEL_EMERGENCY; i++)
    {
      if (strcmp(s, g_level_str[i]) == 0)
        {
          *out = (vg_level_t)i;
          return 0;
        }
    }

  return -1;
}

int vg_status_parse(const char *s, vg_local_status_t *out)
{
  int i;

  if (s == NULL || out == NULL)
    {
      return -1;
    }

  for (i = 0; i <= VG_STATUS_NO_RESPONSE; i++)
    {
      if (strcmp(s, g_status_str[i]) == 0)
        {
          *out = (vg_local_status_t)i;
          return 0;
        }
    }

  return -1;
}

int vg_voice_kind_parse(const char *s, vg_voice_kind_t *out)
{
  int i;

  if (s == NULL || out == NULL)
    {
      return -1;
    }

  for (i = 0; i < VG_VOICE_KIND_MAX; i++)
    {
      if (strcmp(s, g_voice_kind_str[i]) == 0)
        {
          *out = (vg_voice_kind_t)i;
          return 0;
        }
    }

  return -1;
}

int vg_event_priority(const vg_safety_event_t *evt)
{
  if (evt == NULL)
    {
      return -1;
    }

  /* 等级权重压倒类别权重 */

  return (int)evt->level * 100 + g_type_priority[evt->type];
}

int vg_event_cmp_priority(const vg_safety_event_t *a,
                          const vg_safety_event_t *b)
{
  int pa = vg_event_priority(a);
  int pb = vg_event_priority(b);

  if (pa != pb)
    {
      return pa - pb;
    }

  /* 同优先级时先发生的事件先展示 */

  if (a->started_at != b->started_at)
    {
      return a->started_at < b->started_at ? 1 : -1;
    }

  return 0;
}

void vg_format_time(int64_t unix_sec, char *buf, size_t len)
{
  /* 固定使用 +08:00 展示，避免依赖板端 tzdata */

  time_t t = (time_t)(unix_sec + 8 * 3600);
  struct tm tm_buf;

  if (buf == NULL || len == 0)
    {
      return;
    }

  if (gmtime_r(&t, &tm_buf) == NULL)
    {
      snprintf(buf, len, "1970-01-01T08:00:00+08:00");
      return;
    }

  /* 取模保证每个字段位宽固定，避免 -Wformat-truncation 误判 */

  snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d+08:00",
           (tm_buf.tm_year + 1900) % 10000, (tm_buf.tm_mon + 1) % 100,
           tm_buf.tm_mday % 100, tm_buf.tm_hour % 100,
           tm_buf.tm_min % 100, tm_buf.tm_sec % 100);
}

int vg_event_to_json(const vg_safety_event_t *evt, char *buf, size_t len)
{
  char timestr[VG_TIMESTR_LEN];
  size_t pos = 0;
  int n;

  if (evt == NULL || buf == NULL || len < 64)
    {
      return -1;
    }

  vg_format_time(evt->started_at, timestr, sizeof(timestr));

  n = snprintf(buf, len,
               "{\"eventId\":\"%s\",\"deviceId\":\"%s\","
               "\"eventType\":\"%s\",\"level\":\"%s\","
               "\"confidence\":%.2f,\"startedAt\":\"%s\","
               "\"durationSec\":%" PRIu32 ",\"localStatus\":\"%s\","
               "\"uploadReason\":\"%s\",\"timeReliable\":%s,"
               "\"night\":%s",
               evt->event_id, evt->device_id,
               vg_event_type_str(evt->type), vg_level_str(evt->level),
               (double)evt->confidence, timestr,
               evt->duration_sec, vg_status_str(evt->local_status),
               vg_upload_reason_str(evt->upload_reason),
               evt->time_reliable ? "true" : "false",
               evt->night ? "true" : "false");

  if (n < 0 || (size_t)n >= len)
    {
      return -1;
    }

  pos = (size_t)n;

  if (evt->matched_phrase[0] != '\0')
    {
      pos += (size_t)snprintf(buf + pos, len - pos, ",\"matchedPhrase\":");
      pos = vg_json_put_escaped(buf, len, pos, evt->matched_phrase);
    }

  if (evt->person_label[0] != '\0')
    {
      pos += (size_t)snprintf(buf + pos, len - pos, ",\"personLabel\":");
      pos = vg_json_put_escaped(buf, len, pos, evt->person_label);
    }

  if (evt->type == VG_EVT_NAME_CALL_HELP ||
      evt->type == VG_EVT_DISTRESS_VOICE)
    {
      pos += (size_t)snprintf(buf + pos, len - pos,
                              ",\"urgency\":%.2f,\"repeatCount\":%u",
                              (double)evt->urgency,
                              (unsigned)evt->repeat_count);
    }

  if (pos >= len)
    {
      return -1;
    }

  pos += (size_t)snprintf(buf + pos, len - pos, ",\"summary\":");
  pos = vg_json_put_escaped(buf, len, pos, evt->summary);

  if (pos + 2 >= len)
    {
      return -1;
    }

  buf[pos++] = '}';
  buf[pos] = '\0';
  return (int)pos;
}

/* 解析 ISO8601 "YYYY-MM-DDTHH:MM:SS+08:00" 为 Unix 秒 */

static int64_t vg_parse_time(const char *s)
{
  int y;
  int mo;
  int d;
  int h;
  int mi;
  int se;
  int off_h = 8;
  int off_m = 0;
  char sign = '+';
  int64_t days;
  static const int cum[12] =
    {
      0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

  if (s == NULL)
    {
      return 0;
    }

  if (sscanf(s, "%d-%d-%dT%d:%d:%d%c%d:%d",
             &y, &mo, &d, &h, &mi, &se, &sign, &off_h, &off_m) < 6)
    {
      return 0;
    }

  if (mo < 1 || mo > 12)
    {
      return 0;
    }

  days = (int64_t)(y - 1970) * 365 + ((y - 1969) / 4) - ((y - 1901) / 100) +
         ((y - 1601) / 400) + cum[mo - 1] + (d - 1);

  if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
    {
      days += 1;
    }

  return days * 86400 + h * 3600 + mi * 60 + se -
         (sign == '-' ? -1 : 1) * (off_h * 3600 + off_m * 60);
}

int vg_event_from_json(const char *json, vg_safety_event_t *evt)
{
  char tmp[VG_TIMESTR_LEN];
  double d;
  int i;

  if (json == NULL || evt == NULL)
    {
      return -1;
    }

  memset(evt, 0, sizeof(*evt));

  if (vg_json_get_str(json, "eventId", evt->event_id,
                      sizeof(evt->event_id)) < 0)
    {
      return -1;
    }

  vg_json_get_str(json, "deviceId", evt->device_id, sizeof(evt->device_id));

  if (vg_json_get_str(json, "eventType", tmp, sizeof(tmp)) < 0 ||
      vg_event_type_parse(tmp, &evt->type) < 0)
    {
      return -1;
    }

  if (vg_json_get_str(json, "level", tmp, sizeof(tmp)) == 0)
    {
      vg_level_parse(tmp, &evt->level);
    }

  if (vg_json_get_str(json, "localStatus", tmp, sizeof(tmp)) == 0)
    {
      vg_status_parse(tmp, &evt->local_status);
    }

  if (vg_json_get_double(json, "confidence", &d) == 0)
    {
      evt->confidence = (float)d;
    }

  if (vg_json_get_str(json, "startedAt", tmp, sizeof(tmp)) == 0)
    {
      evt->started_at = vg_parse_time(tmp);
    }

  if (vg_json_get_int(json, "durationSec", &i) == 0)
    {
      evt->duration_sec = (uint32_t)i;
    }

  if (vg_json_get_int(json, "repeatCount", &i) == 0)
    {
      evt->repeat_count = (uint16_t)i;
    }

  if (vg_json_get_double(json, "urgency", &d) == 0)
    {
      evt->urgency = (float)d;
    }

  vg_json_get_bool(json, "timeReliable", &evt->time_reliable);
  vg_json_get_bool(json, "night", &evt->night);
  vg_json_get_str(json, "matchedPhrase", evt->matched_phrase,
                  sizeof(evt->matched_phrase));
  vg_json_get_str(json, "personLabel", evt->person_label,
                  sizeof(evt->person_label));
  vg_json_get_str(json, "summary", evt->summary, sizeof(evt->summary));

  return 0;
}

int vg_event_validate(const vg_safety_event_t *evt)
{
  if (evt == NULL)
    {
      return -1;
    }

  if (evt->event_id[0] == '\0')
    {
      return -2;
    }

  if (evt->device_id[0] == '\0')
    {
      return -3;
    }

  if (evt->type < 0 || evt->type >= VG_EVT_TYPE_MAX)
    {
      return -4;
    }

  if (evt->level < VG_LEVEL_NOTICE || evt->level > VG_LEVEL_EMERGENCY)
    {
      return -5;
    }

  if (evt->confidence < 0.0f || evt->confidence > 1.0f)
    {
      return -6;
    }

  if (evt->local_status < 0 || evt->local_status > VG_STATUS_NO_RESPONSE)
    {
      return -7;
    }

  if (evt->upload_reason < 0 || evt->upload_reason > VG_UPLOAD_SYNC)
    {
      return -8;
    }

  return 0;
}
