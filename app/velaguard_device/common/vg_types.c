/****************************************************************************
 * 安聆 VelaGuard - 公共类型与结构化事件协议实现 (PRD-05)
 ****************************************************************************/

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
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
  "none", "moan", "scream", "shout_help", "name_call", "help_phrase",
  "distress"
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

static int vg_json_append(char *buf, size_t len, size_t *pos,
                          const char *text)
{
  int n;

  if (buf == NULL || pos == NULL || text == NULL || *pos >= len)
    {
      return -1;
    }

  n = snprintf(buf + *pos, len - *pos, "%s", text);
  if (n < 0 || (size_t)n >= len - *pos)
    {
      return -1;
    }

  *pos += (size_t)n;
  return 0;
}

static int vg_json_append_escaped(char *buf, size_t len, size_t *pos,
                                  const char *text)
{
  size_t next;

  if (buf == NULL || pos == NULL || *pos >= len)
    {
      return -1;
    }

  next = vg_json_put_escaped(buf, len, *pos, text != NULL ? text : "");
  if (next >= len || next <= *pos || buf[next - 1] != '"')
    {
      return -1;
    }

  *pos = next;
  return 0;
}

static int vg_json_append_number(char *buf, size_t len, size_t *pos,
                                 const char *fmt, ...)
{
  va_list ap;
  int n;

  if (buf == NULL || pos == NULL || *pos >= len)
    {
      return -1;
    }

  va_start(ap, fmt);
  n = vsnprintf(buf + *pos, len - *pos, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= len - *pos)
    {
      return -1;
    }

  *pos += (size_t)n;
  return 0;
}

static int vg_json_append_field_string(char *buf, size_t len, size_t *pos,
                                       const char *key, const char *value)
{
  return vg_json_append(buf, len, pos, key) < 0 ||
         vg_json_append_escaped(buf, len, pos, value) < 0 ? -1 : 0;
}

int vg_event_to_json(const vg_safety_event_t *evt, char *buf, size_t len)
{
  char timestr[VG_TIMESTR_LEN];
  size_t pos = 0;

  if (evt == NULL || buf == NULL || len < 64 ||
      vg_event_validate(evt) != 0)
    {
      return -1;
    }

  vg_format_time(evt->started_at, timestr, sizeof(timestr));
  if (vg_json_append(buf, len, &pos, "{\"eventId\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, evt->event_id) < 0 ||
      vg_json_append(buf, len, &pos, ",\"deviceId\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, evt->device_id) < 0 ||
      vg_json_append(buf, len, &pos, ",\"eventType\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, vg_event_type_str(evt->type)) < 0 ||
      vg_json_append(buf, len, &pos, ",\"level\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, vg_level_str(evt->level)) < 0 ||
      vg_json_append(buf, len, &pos, ",\"confidence\":") < 0 ||
      vg_json_append_number(buf, len, &pos, "%.6f",
                            (double)evt->confidence) < 0 ||
      vg_json_append(buf, len, &pos, ",\"startedAt\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, timestr) < 0 ||
      vg_json_append(buf, len, &pos, ",\"durationSec\":") < 0 ||
      vg_json_append_number(buf, len, &pos, "%" PRIu32,
                            evt->duration_sec) < 0 ||
      vg_json_append(buf, len, &pos, ",\"localStatus\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos,
                             vg_status_str(evt->local_status)) < 0 ||
      vg_json_append(buf, len, &pos, ",\"uploadReason\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos,
                             vg_upload_reason_str(evt->upload_reason)) < 0 ||
      vg_json_append(buf, len, &pos, ",\"timeReliable\":") < 0 ||
      vg_json_append(buf, len, &pos,
                     evt->time_reliable ? "true" : "false") < 0 ||
      vg_json_append(buf, len, &pos, ",\"night\":") < 0 ||
      vg_json_append(buf, len, &pos, evt->night ? "true" : "false") < 0)
    {
      return -1;
    }

  if (evt->matched_phrase[0] != '\0' &&
      vg_json_append_field_string(buf, len, &pos,
                                  ",\"matchedPhrase\":",
                                  evt->matched_phrase) < 0)
    {
      return -1;
    }

  if (evt->person_label[0] != '\0' &&
      vg_json_append_field_string(buf, len, &pos,
                                  ",\"personLabel\":",
                                  evt->person_label) < 0)
    {
      return -1;
    }

  if (evt->type == VG_EVT_NAME_CALL_HELP ||
      evt->type == VG_EVT_DISTRESS_VOICE)
    {
      if (vg_json_append(buf, len, &pos, ",\"urgency\":") < 0 ||
          vg_json_append_number(buf, len, &pos, "%.6f",
                                (double)evt->urgency) < 0 ||
          vg_json_append(buf, len, &pos, ",\"repeatCount\":") < 0 ||
          vg_json_append_number(buf, len, &pos, "%u",
                                (unsigned)evt->repeat_count) < 0)
        {
          return -1;
        }
    }

  if (vg_json_append(buf, len, &pos, ",\"summary\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, evt->summary) < 0 ||
      vg_json_append(buf, len, &pos, "}") < 0)
    {
      return -1;
    }

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
  bool bool_value;

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

  if (vg_json_get_str(json, "deviceId", evt->device_id,
                      sizeof(evt->device_id)) < 0)
    {
      return -1;
    }

  if (vg_json_get_str(json, "eventType", tmp, sizeof(tmp)) < 0 ||
      vg_event_type_parse(tmp, &evt->type) < 0)
    {
      return -1;
    }

  if (vg_json_get_str(json, "level", tmp, sizeof(tmp)) < 0 ||
      vg_level_parse(tmp, &evt->level) < 0)
    {
      return -1;
    }

  if (vg_json_get_str(json, "localStatus", tmp, sizeof(tmp)) < 0 ||
      vg_status_parse(tmp, &evt->local_status) < 0)
    {
      return -1;
    }

  if (vg_json_get_double(json, "confidence", &d) < 0 ||
      !isfinite(d))
    {
      return -1;
    }
  evt->confidence = (float)d;

  if (vg_json_get_str(json, "startedAt", tmp, sizeof(tmp)) < 0)
    {
      return -1;
    }
  evt->started_at = vg_parse_time(tmp);

  if (vg_json_get_int(json, "durationSec", &i) < 0 || i < 0)
    {
      return -1;
    }
  evt->duration_sec = (uint32_t)i;

  if (vg_json_get_int(json, "repeatCount", &i) == 0 && i >= 0)
    {
      evt->repeat_count = (uint16_t)i;
    }

  if (vg_json_get_double(json, "urgency", &d) == 0 && isfinite(d))
    {
      evt->urgency = (float)d;
    }

  if (vg_json_get_bool(json, "timeReliable", &bool_value) < 0)
    {
      return -1;
    }
  evt->time_reliable = bool_value;

  if (vg_json_get_bool(json, "night", &bool_value) < 0)
    {
      return -1;
    }
  evt->night = bool_value;

  vg_json_get_str(json, "matchedPhrase", evt->matched_phrase,
                  sizeof(evt->matched_phrase));
  vg_json_get_str(json, "personLabel", evt->person_label,
                  sizeof(evt->person_label));
  if (vg_json_get_str(json, "summary", evt->summary,
                      sizeof(evt->summary)) < 0)
    {
      return -1;
    }

  if (vg_json_get_int(json, "eventRevision", &i) == 0 && i > 0)
    {
      evt->event_revision = (uint32_t)i;
    }

  return 0;
}

static bool vg_protocol_id_valid(const char *s, size_t capacity)
{
  size_t i;
  size_t n;

  if (s == NULL || capacity == 0 ||
      memchr(s, '\0', capacity) == NULL)
    {
      return false;
    }

  n = strlen(s);
  if (n == 0 || n >= capacity)
    {
      return false;
    }

  for (i = 0; i < n; i++)
    {
      unsigned char c = (unsigned char)s[i];

      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
        {
          return false;
        }
    }

  return true;
}

static bool vg_protocol_text_valid(const char *s, size_t capacity)
{
  size_t i;

  if (s == NULL || capacity == 0 ||
      memchr(s, '\0', capacity) == NULL)
    {
      return false;
    }

  for (i = 0; s[i] != '\0'; i++)
    {
      if ((unsigned char)s[i] < 0x20)
        {
          return false;
        }
    }

  return true;
}

int vg_event_validate(const vg_safety_event_t *evt)
{
  if (evt == NULL)
    {
      return -1;
    }

  if (!vg_protocol_id_valid(evt->event_id, sizeof(evt->event_id)))
    {
      return -2;
    }

  if (!vg_protocol_id_valid(evt->device_id, sizeof(evt->device_id)))
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

  if (!isfinite(evt->confidence) ||
      evt->confidence < 0.0f || evt->confidence > 1.0f)
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

  if (evt->started_at < 0 || evt->duration_sec > 7 * 24 * 60 * 60)
    {
      return -9;
    }

  if (!isfinite(evt->urgency) || evt->urgency < 0.0f ||
      evt->urgency > 1.0f || evt->repeat_count > 10000)
    {
      return -10;
    }

  if (!vg_protocol_text_valid(evt->matched_phrase,
                              sizeof(evt->matched_phrase)) ||
      !vg_protocol_text_valid(evt->person_label,
                              sizeof(evt->person_label)) ||
      !vg_protocol_text_valid(evt->summary, sizeof(evt->summary)))
    {
      return -11;
    }

  return 0;
}

int vg_event_envelope_to_json(const vg_safety_event_t *evt,
                              const char *advice,
                              const char *message_id,
                              const char *device_epoch,
                              uint64_t device_seq,
                              uint32_t event_revision,
                              const char *trace_id,
                              uint64_t monotonic_ms,
                              char *buf, size_t len)
{
  char payload[1536];
  char sent_at[VG_TIMESTR_LEN];
  size_t payload_len;
  size_t pos = 0;

  if (evt == NULL || buf == NULL || message_id == NULL ||
      device_epoch == NULL || trace_id == NULL || len < 256 ||
      vg_event_validate(evt) != 0 || device_seq == 0 ||
      event_revision == 0 ||
      !vg_protocol_id_valid(message_id, VG_MESSAGE_ID_LEN) ||
      !vg_protocol_id_valid(device_epoch, VG_DEVICE_EPOCH_LEN) ||
      !vg_protocol_id_valid(trace_id, VG_TRACE_ID_LEN))
    {
      return -1;
    }

  if (vg_event_to_json(evt, payload, sizeof(payload)) < 0)
    {
      return -1;
    }

  payload_len = strlen(payload);
  if (payload_len < 2 || payload[payload_len - 1] != '}')
    {
      return -1;
    }

  payload[payload_len - 1] = '\0';
  payload_len--;
  if (vg_json_append(payload, sizeof(payload), &payload_len,
                     ",\"advice\":") < 0 ||
      vg_json_append_escaped(payload, sizeof(payload), &payload_len,
                             advice != NULL ? advice : "") < 0 ||
      vg_json_append(payload, sizeof(payload), &payload_len, "}") < 0)
    {
      return -1;
    }

  vg_format_time(time(NULL), sent_at, sizeof(sent_at));
  if (vg_json_append(buf, len, &pos, "{\"protocol\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, VG_PROTOCOL_NAME) < 0 ||
      vg_json_append(buf, len, &pos, ",\"schema\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, VG_EVENT_SCHEMA) < 0 ||
      vg_json_append(buf, len, &pos, ",\"messageType\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, VG_EVENT_MESSAGE) < 0 ||
      vg_json_append(buf, len, &pos, ",\"messageId\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, message_id) < 0 ||
      vg_json_append(buf, len, &pos, ",\"deviceId\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, evt->device_id) < 0 ||
      vg_json_append(buf, len, &pos, ",\"deviceEpoch\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, device_epoch) < 0 ||
      vg_json_append(buf, len, &pos, ",\"deviceSeq\":") < 0 ||
      vg_json_append_number(buf, len, &pos, "%" PRIu64, device_seq) < 0 ||
      vg_json_append(buf, len, &pos, ",\"eventId\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, evt->event_id) < 0 ||
      vg_json_append(buf, len, &pos, ",\"eventRevision\":") < 0 ||
      vg_json_append_number(buf, len, &pos, "%" PRIu32,
                            event_revision) < 0 ||
      vg_json_append(buf, len, &pos, ",\"sentAt\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, sent_at) < 0 ||
      vg_json_append(buf, len, &pos, ",\"monotonicMs\":") < 0 ||
      vg_json_append_number(buf, len, &pos, "%" PRIu64, monotonic_ms) < 0 ||
      vg_json_append(buf, len, &pos, ",\"traceId\":") < 0 ||
      vg_json_append_escaped(buf, len, &pos, trace_id) < 0 ||
      vg_json_append(buf, len, &pos, ",\"payload\":") < 0 ||
      vg_json_append(buf, len, &pos, payload) < 0 ||
      vg_json_append(buf, len, &pos, "}") < 0)
    {
      return -1;
    }

  return (int)pos;
}

int vg_event_envelope_from_json(const char *json,
                                vg_event_envelope_t *envelope)
{
  char tmp[VG_TIMESTR_LEN];
  long long device_seq;
  long long monotonic_ms;
  int value;

  if (json == NULL || envelope == NULL)
    {
      return -1;
    }

  memset(envelope, 0, sizeof(*envelope));
  if (vg_json_get_str(json, "protocol", envelope->protocol,
                      sizeof(envelope->protocol)) < 0 ||
      vg_json_get_str(json, "schema", envelope->schema,
                      sizeof(envelope->schema)) < 0 ||
      vg_json_get_str(json, "messageType", envelope->message_type,
                      sizeof(envelope->message_type)) < 0 ||
      vg_json_get_str(json, "messageId", envelope->message_id,
                      sizeof(envelope->message_id)) < 0 ||
      vg_json_get_str(json, "deviceId", envelope->device_id,
                      sizeof(envelope->device_id)) < 0 ||
      vg_json_get_str(json, "deviceEpoch", envelope->device_epoch,
                      sizeof(envelope->device_epoch)) < 0 ||
      vg_json_get_str(json, "eventId", envelope->event_id,
                      sizeof(envelope->event_id)) < 0 ||
      vg_json_get_str(json, "sentAt", tmp, sizeof(tmp)) < 0 ||
      vg_json_get_str(json, "traceId", envelope->trace_id,
                      sizeof(envelope->trace_id)) < 0 ||
      vg_json_get_i64(json, "deviceSeq", &device_seq) < 0 ||
      device_seq <= 0 ||
      vg_json_get_i64(json, "monotonicMs", &monotonic_ms) < 0 ||
      monotonic_ms < 0 ||
      vg_json_get_int(json, "eventRevision", &value) < 0 ||
      value <= 0)
    {
      return -1;
    }

  envelope->device_seq = (uint64_t)device_seq;
  envelope->monotonic_ms = (uint64_t)monotonic_ms;
  envelope->event_revision = (uint32_t)value;
  envelope->sent_at = vg_parse_time(tmp);

  if (vg_event_from_json(json, &envelope->event) < 0 ||
      vg_json_get_str(json, "advice", envelope->advice,
                      sizeof(envelope->advice)) < 0)
    {
      return -1;
    }

  return vg_event_envelope_validate(envelope);
}

int vg_event_envelope_validate(const vg_event_envelope_t *envelope)
{
  if (envelope == NULL ||
      strcmp(envelope->protocol, VG_PROTOCOL_NAME) != 0 ||
      strcmp(envelope->schema, VG_EVENT_SCHEMA) != 0 ||
      strcmp(envelope->message_type, VG_EVENT_MESSAGE) != 0 ||
      !vg_protocol_id_valid(envelope->message_id,
                            sizeof(envelope->message_id)) ||
      !vg_protocol_id_valid(envelope->device_id,
                            sizeof(envelope->device_id)) ||
      !vg_protocol_id_valid(envelope->device_epoch,
                            sizeof(envelope->device_epoch)) ||
      !vg_protocol_id_valid(envelope->event_id,
                            sizeof(envelope->event_id)) ||
      !vg_protocol_id_valid(envelope->trace_id,
                            sizeof(envelope->trace_id)) ||
      envelope->device_seq == 0 || envelope->event_revision == 0 ||
      envelope->sent_at < 0 ||
      strcmp(envelope->device_id, envelope->event.device_id) != 0 ||
      strcmp(envelope->event_id, envelope->event.event_id) != 0 ||
      (envelope->event.event_revision != 0 &&
       envelope->event.event_revision != envelope->event_revision) ||
      !vg_protocol_text_valid(envelope->advice,
                              sizeof(envelope->advice)) ||
      vg_event_validate(&envelope->event) != 0)
    {
      return -1;
    }

  return 0;
}
