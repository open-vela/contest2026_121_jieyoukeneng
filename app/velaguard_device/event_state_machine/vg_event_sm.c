/****************************************************************************
 * 安聆 VelaGuard - 事件状态机与告警规则实现 (PRD-03)
 ****************************************************************************/

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "velaguard/vg_config.h"
#include "velaguard/vg_event_log.h"
#include "velaguard/vg_event_sm.h"
#include "velaguard/vg_time.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct
{
  bool              active;
  vg_state_t        state;
  vg_safety_event_t evt;
  bool              materialized;    /* 是否已生成 eventId 并写日志 */
  bool              local_only;      /* 平静单次呼喊：只本地确认不升级 */
  bool              notified;        /* 已触发远程通知 */

  uint64_t          first_hit_ms;
  uint64_t          last_hit_ms;
  uint64_t          state_since_ms;
  uint64_t          deadline_ms;     /* 警告倒计时截止 */
  uint64_t          snooze_deadline_ms;

  uint32_t          snooze_count;
  uint32_t          hit_count;       /* 连续命中窗口数 */
  uint32_t          repeat_count;    /* 瞬态事件重复次数 */
  uint64_t          last_repeat_ms;

  float             conf_peak;
  float             urgency_peak;
} vg_track_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_track_t g_tracks[VG_EVT_TYPE_MAX];
static vg_sm_cb_t g_cb;
static uint32_t   g_seq;
static bool       g_inited;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void vg_notify_changed(vg_track_t *t)
{
  if (t->materialized)
    {
      vg_event_log_put(&t->evt);
    }

  if (g_cb.on_event_changed != NULL && t->materialized)
    {
      g_cb.on_event_changed(&t->evt, t->state, g_cb.arg);
    }
}

static void vg_notify_escalate(vg_track_t *t)
{
  if (g_cb.on_escalate != NULL)
    {
      g_cb.on_escalate(&t->evt, g_cb.arg);
    }

  t->notified = true;
}

static void vg_notify_closed(vg_track_t *t)
{
  if (g_cb.on_closed != NULL && t->materialized)
    {
      g_cb.on_closed(&t->evt, g_cb.arg);
    }
}

/* 生成事件摘要（结构化描述，绝不含原始音频或对话文本） */

static void vg_build_summary(vg_track_t *t)
{
  const char *night = t->evt.night ? "夜间" : "";
  vg_safety_event_t *e = &t->evt;

  switch (e->type)
    {
      case VG_EVT_ALARM_BEEP:
        snprintf(e->summary, sizeof(e->summary),
                 "%s检测到疑似烟雾或燃气报警蜂鸣，持续 %" PRIu32 " 秒",
                 night, e->duration_sec);
        break;

      case VG_EVT_WATER_FLOW:
        snprintf(e->summary, sizeof(e->summary),
                 "%s检测到持续水流声，已持续 %" PRIu32 " 秒，疑似忘关水龙头或漏水",
                 night, e->duration_sec);
        break;

      case VG_EVT_IMPACT:
        snprintf(e->summary, sizeof(e->summary),
                 "%s检测到玻璃破碎或重物撞击声%s",
                 night,
                 t->repeat_count > 1 ? "，短时间内重复出现" : "");
        break;

      case VG_EVT_DISTRESS_VOICE:
        snprintf(e->summary, sizeof(e->summary),
                 "%s检测到呻吟或痛苦呼救声，重复 %u 次，%s",
                 night, (unsigned)e->repeat_count,
                 e->local_status == VG_STATUS_NO_RESPONSE ?
                 "用户未确认" : "等待用户确认");
        break;

      case VG_EVT_NAME_CALL_HELP:
        {
          const char *tone = (e->urgency >= 0.5f) ? "急促" : "平静";
          const char *wait = (e->local_status == VG_STATUS_NO_RESPONSE) ?
                             "用户未确认" : "等待用户确认";

          if (e->matched_phrase[0] != '\0')
            {
              snprintf(e->summary, sizeof(e->summary),
                       "%s检测到%s呼喊「%s」，重复 %u 次，%s",
                       night, tone, e->matched_phrase,
                       (unsigned)e->repeat_count, wait);
            }
          else
            {
              snprintf(e->summary, sizeof(e->summary),
                       "%s检测到%s呼喊求助，重复 %u 次，%s",
                       night, tone, (unsigned)e->repeat_count, wait);
            }
        }
        break;

      default:
        snprintf(e->summary, sizeof(e->summary), "检测到异常声音事件");
        break;
    }
}

static void vg_materialize(vg_track_t *t)
{
  vg_config_t *cfg = vg_config();

  if (t->materialized)
    {
      return;
    }

  snprintf(t->evt.event_id, sizeof(t->evt.event_id),
           "evt_%06" PRIu32, ++g_seq);
  vg_strlcpy(t->evt.device_id, cfg->device_id, sizeof(t->evt.device_id));
  t->evt.local_status = VG_STATUS_PENDING;
  t->evt.upload_reason = VG_UPLOAD_SYNC;
  t->materialized = true;
}

/* 等级提升（只升不降），并处理各级伴随动作 */

static void vg_set_level(vg_track_t *t, vg_level_t level)
{
  vg_config_t *cfg = vg_config();
  uint64_t now = vg_now_ms();

  if (level <= t->evt.level && t->materialized)
    {
      return;
    }

  vg_materialize(t);
  t->evt.level = level;
  t->state_since_ms = now;

  switch (level)
    {
      case VG_LEVEL_NOTICE:
        t->state = VG_STATE_NOTICE;
        t->deadline_ms = 0;
        break;

      case VG_LEVEL_WARNING:
        t->state = VG_STATE_WARNING;
        t->deadline_ms = now + (uint64_t)cfg->warning_countdown_sec * 1000;
        break;

      case VG_LEVEL_EMERGENCY:
        t->state = VG_STATE_EMERGENCY;
        t->deadline_ms = 0;
        break;

      default:
        break;
    }

  vg_build_summary(t);
  vg_notify_changed(t);

  if (level == VG_LEVEL_EMERGENCY && !t->notified && !t->local_only)
    {
      t->evt.upload_reason = VG_UPLOAD_ESCALATED;
      vg_notify_escalate(t);
    }
}

static void vg_track_close(vg_track_t *t, vg_local_status_t status)
{
  t->evt.local_status = status;
  t->state = VG_STATE_CLOSED;
  vg_build_summary(t);
  vg_notify_changed(t);
  vg_notify_closed(t);
  t->active = false;
}

static void vg_track_start(vg_track_t *t, vg_event_type_t type, uint64_t now)
{
  memset(t, 0, sizeof(*t));
  t->active = true;
  t->state = VG_STATE_SUSPECT;
  t->first_hit_ms = now;
  t->last_hit_ms = now;
  t->state_since_ms = now;
  t->evt.type = type;
  t->evt.level = VG_LEVEL_NONE;
  t->evt.started_at = vg_wall_sec();
  t->evt.time_reliable = vg_time_reliable();
  t->evt.night = vg_is_night();
  t->evt.local_status = VG_STATUS_PENDING;
}

static void vg_track_update_duration(vg_track_t *t)
{
  t->evt.duration_sec =
      (uint32_t)((t->last_hit_ms - t->first_hit_ms) / 1000);
}

/****************************************************************************
 * 五类事件规则 (PRD-03)
 ****************************************************************************/

static void vg_rule_alarm_beep(vg_track_t *t)
{
  vg_config_t *cfg = vg_config();
  uint64_t held = t->last_hit_ms - t->first_hit_ms;

  /* 高置信度连续约 5 秒（多个连续窗口确认）-> 直接紧急级 */

  if (held >= (uint64_t)cfg->alarm_hold_sec * 1000 && t->hit_count >= 3)
    {
      vg_set_level(t, VG_LEVEL_EMERGENCY);
    }
}

static void vg_rule_water_flow(vg_track_t *t)
{
  vg_config_t *cfg = vg_config();
  uint64_t held = t->last_hit_ms - t->first_hit_ms;

  if (held >= (uint64_t)cfg->water_warning_sec * 1000)
    {
      vg_set_level(t, VG_LEVEL_WARNING);
    }
  else if (held >= (uint64_t)cfg->water_notice_sec * 1000)
    {
      vg_set_level(t, VG_LEVEL_NOTICE);
    }
}

static void vg_rule_impact(vg_track_t *t)
{
  /* 单次高置信度瞬态 -> 警告；夜间或 60s 内重复 2 次 -> 紧急 */

  if (t->evt.night || t->repeat_count >= 2)
    {
      vg_set_level(t, VG_LEVEL_EMERGENCY);
    }
  else
    {
      vg_set_level(t, VG_LEVEL_WARNING);
    }
}

static void vg_rule_distress(vg_track_t *t)
{
  /* 先本地确认；重复命中或无人响应 -> 升级 */

  if (t->repeat_count >= 2)
    {
      vg_set_level(t, VG_LEVEL_EMERGENCY);
    }
  else
    {
      vg_set_level(t, VG_LEVEL_WARNING);
    }
}

static void vg_rule_name_call(vg_track_t *t)
{
  bool distress_active = g_tracks[VG_EVT_DISTRESS_VOICE].active &&
                         g_tracks[VG_EVT_DISTRESS_VOICE].state !=
                         VG_STATE_CLOSED;

  /* 平静、单次呼喊姓名 -> 只本地确认，不远程通知（PRD-02/03）
   * 急促重复呼喊 / 明确求救词 / 与异常人声组合 -> 进入升级路径
   */

  if (t->local_only)
    {
      bool urgent_repeat = (t->urgency_peak >= 0.5f) &&
                           (t->repeat_count >= 2);

      if (urgent_repeat || distress_active)
        {
          t->local_only = false;
        }
      else
        {
          vg_set_level(t, VG_LEVEL_NOTICE);
          return;
        }
    }

  if (t->repeat_count >= 3 || distress_active)
    {
      vg_set_level(t, VG_LEVEL_EMERGENCY);
    }
  else
    {
      vg_set_level(t, VG_LEVEL_WARNING);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void vg_sm_set_callbacks(const vg_sm_cb_t *cb)
{
  memset(&g_cb, 0, sizeof(g_cb));
  if (cb != NULL)
    {
      g_cb = *cb;
    }

  g_inited = true;
}

void vg_sm_init(const vg_sm_cb_t *cb)
{
  vg_sm_set_callbacks(cb);
  memset(g_tracks, 0, sizeof(g_tracks));
  g_seq = vg_event_log_total();
}

void vg_sm_reset(void)
{
  memset(g_tracks, 0, sizeof(g_tracks));
  g_seq = vg_event_log_total();
}

void vg_sm_feed_sound(const vg_sound_obs_t *obs)
{
  vg_config_t *cfg = vg_config();
  vg_event_type_t type;
  vg_track_t *t;
  float threshold;
  uint64_t now;

  if (!g_inited || obs == NULL)
    {
      return;
    }

  switch (obs->cls)
    {
      case VG_SOUND_ALARM_BEEP:
        type = VG_EVT_ALARM_BEEP;
        threshold = cfg->conf_alarm_beep;
        break;

      case VG_SOUND_WATER_FLOW:
        type = VG_EVT_WATER_FLOW;
        threshold = cfg->conf_water_flow;
        break;

      case VG_SOUND_IMPACT:
        type = VG_EVT_IMPACT;
        threshold = cfg->conf_impact;
        break;

      default:
        return;
    }

  if (obs->confidence < threshold)
    {
      return;
    }

  now = (obs->ts_ms != 0) ? obs->ts_ms : vg_now_ms();
  t = &g_tracks[type];

  if (!t->active || t->state == VG_STATE_CLOSED)
    {
      /* 已确认为"误报/已处理"的事件在静默期内不重复起新事件 */

      if (t->state == VG_STATE_CLOSED &&
          now - t->state_since_ms <
              (uint64_t)cfg->event_idle_timeout_sec * 1000)
        {
          return;
        }

      vg_track_start(t, type, now);
    }

  t->last_hit_ms = now;
  t->hit_count++;

  if (obs->confidence > t->conf_peak)
    {
      t->conf_peak = obs->confidence;
    }

  t->evt.confidence = t->conf_peak;
  vg_track_update_duration(t);

  if (type == VG_EVT_IMPACT)
    {
      if (t->last_repeat_ms == 0 ||
          now - t->last_repeat_ms <=
              (uint64_t)cfg->impact_repeat_window_sec * 1000)
        {
          t->repeat_count++;
        }
      else
        {
          t->repeat_count = 1;
        }

      t->last_repeat_ms = now;
      t->evt.repeat_count = (uint16_t)t->repeat_count;
    }

  if (t->state == VG_STATE_SNOOZED)
    {
      /* snooze 期间继续统计，但不重新提醒 */

      return;
    }

  switch (type)
    {
      case VG_EVT_ALARM_BEEP:
        vg_rule_alarm_beep(t);
        break;

      case VG_EVT_WATER_FLOW:
        vg_rule_water_flow(t);
        break;

      case VG_EVT_IMPACT:
        vg_rule_impact(t);
        break;

      default:
        break;
    }
}

void vg_sm_feed_distress(const vg_distress_obs_t *obs)
{
  vg_config_t *cfg = vg_config();
  vg_event_type_t type;
  vg_track_t *t;
  float threshold;
  uint64_t now;
  bool calm_single;

  if (!g_inited || obs == NULL || obs->kind == VG_VOICE_NONE)
    {
      return;
    }

  if (obs->kind == VG_VOICE_NAME_CALL || obs->kind == VG_VOICE_HELP_PHRASE)
    {
      type = VG_EVT_NAME_CALL_HELP;
      threshold = cfg->conf_name_call;
    }
  else
    {
      type = VG_EVT_DISTRESS_VOICE;
      threshold = cfg->conf_distress;
    }

  if (obs->confidence < threshold)
    {
      return;
    }

  now = (obs->ts_ms != 0) ? obs->ts_ms : vg_now_ms();
  t = &g_tracks[type];

  if (!t->active || t->state == VG_STATE_CLOSED)
    {
      if (t->state == VG_STATE_CLOSED &&
          now - t->state_since_ms <
              (uint64_t)cfg->event_idle_timeout_sec * 1000)
        {
          return;
        }

      vg_track_start(t, type, now);

      /* 明确求救词一开始就不属于"平静单次呼喊" */

      calm_single = (obs->kind == VG_VOICE_NAME_CALL) &&
                    (obs->urgency < 0.5f);
      t->local_only = (type == VG_EVT_NAME_CALL_HELP) && calm_single;
    }

  t->last_hit_ms = now;
  t->hit_count++;

  if (t->last_repeat_ms == 0 ||
      now - t->last_repeat_ms <=
          (uint64_t)cfg->distress_repeat_window_sec * 1000)
    {
      t->repeat_count += (obs->repeat_count > 0) ? obs->repeat_count : 1;
    }
  else
    {
      t->repeat_count = (obs->repeat_count > 0) ? obs->repeat_count : 1;
    }

  t->last_repeat_ms = now;

  if (obs->confidence > t->conf_peak)
    {
      t->conf_peak = obs->confidence;
    }

  if (obs->urgency > t->urgency_peak)
    {
      t->urgency_peak = obs->urgency;
    }

  t->evt.confidence = t->conf_peak;
  t->evt.urgency = t->urgency_peak;
  t->evt.repeat_count = (uint16_t)t->repeat_count;

  if (obs->matched_phrase[0] != '\0')
    {
      vg_strlcpy(t->evt.matched_phrase, obs->matched_phrase, sizeof(t->evt.matched_phrase));
    }

  if (obs->person_label[0] != '\0')
    {
      vg_strlcpy(t->evt.person_label, obs->person_label, sizeof(t->evt.person_label));
    }

  /* 明确求救短语（含方言）不受"平静单次"豁免 */

  if (obs->kind == VG_VOICE_HELP_PHRASE)
    {
      t->local_only = false;
    }

  vg_track_update_duration(t);

  if (t->state == VG_STATE_SNOOZED)
    {
      return;
    }

  if (type == VG_EVT_NAME_CALL_HELP)
    {
      vg_rule_name_call(t);
    }
  else
    {
      vg_rule_distress(t);
    }
}

void vg_sm_tick(void)
{
  vg_config_t *cfg = vg_config();
  uint64_t now = vg_now_ms();
  int i;

  if (!g_inited)
    {
      return;
    }

  for (i = 0; i < VG_EVT_TYPE_MAX; i++)
    {
      vg_track_t *t = &g_tracks[i];

      if (!t->active)
        {
          continue;
        }

      switch (t->state)
        {
          case VG_STATE_SUSPECT:
            /* 长时间无新命中且未达任何等级 -> 丢弃 */

            if (now - t->last_hit_ms >
                (uint64_t)cfg->event_idle_timeout_sec * 1000)
              {
                t->active = false;
              }
            break;

          case VG_STATE_NOTICE:
            /* 提醒级不做远程升级；长时间无新命中自动收敛 */

            if (now - t->last_hit_ms >
                (uint64_t)cfg->event_idle_timeout_sec * 1000 * 4)
              {
                vg_track_close(t, t->evt.local_status == VG_STATUS_PENDING ?
                                  VG_STATUS_NO_RESPONSE :
                                  t->evt.local_status);
              }
            else if (t->evt.type == VG_EVT_WATER_FLOW)
              {
                vg_track_update_duration(t);
                vg_rule_water_flow(t);
              }
            break;

          case VG_STATE_WARNING:
            if (t->evt.type == VG_EVT_WATER_FLOW &&
                now - t->last_hit_ms <=
                    (uint64_t)cfg->event_idle_timeout_sec * 1000)
              {
                vg_track_update_duration(t);
              }

            /* 倒计时结束、无人确认 -> no_response -> 紧急级 (PRD-03/06) */

            if (t->deadline_ms != 0 && now >= t->deadline_ms)
              {
                t->evt.local_status = VG_STATUS_NO_RESPONSE;

                if (t->local_only)
                  {
                    /* 平静单次呼喊：超时也只关闭，不做远程通知 */

                    vg_track_close(t, VG_STATUS_NO_RESPONSE);
                  }
                else
                  {
                    vg_set_level(t, VG_LEVEL_EMERGENCY);
                  }
              }
            break;

          case VG_STATE_SNOOZED:
            if (now >= t->snooze_deadline_ms)
              {
                /* 到期重新进入警告级倒计时 */

                t->evt.local_status = VG_STATUS_PENDING;
                t->state = VG_STATE_WARNING;
                t->state_since_ms = now;
                t->deadline_ms =
                    now + (uint64_t)cfg->warning_countdown_sec * 1000;
                vg_notify_changed(t);
              }
            break;

          case VG_STATE_EMERGENCY:
            /* 紧急级持续本地提醒，等待用户确认，不自动关闭 */

            if (t->evt.type == VG_EVT_WATER_FLOW &&
                now - t->last_hit_ms <=
                    (uint64_t)cfg->event_idle_timeout_sec * 1000)
              {
                vg_track_update_duration(t);
              }
            break;

          default:
            break;
        }
    }
}

int vg_sm_ack(const char *event_id, vg_local_status_t action)
{
  vg_config_t *cfg = vg_config();
  vg_track_t *t = NULL;
  uint64_t now = vg_now_ms();
  int i;

  if (event_id == NULL || event_id[0] == '\0')
    {
      vg_track_view_t view;

      if (vg_sm_top(&view) < 0)
        {
          return -1;
        }

      event_id = view.evt.event_id;
    }

  for (i = 0; i < VG_EVT_TYPE_MAX; i++)
    {
      if (g_tracks[i].active && g_tracks[i].materialized &&
          strcmp(g_tracks[i].evt.event_id, event_id) == 0)
        {
          t = &g_tracks[i];
          break;
        }
    }

  if (t == NULL)
    {
      return -1;
    }

  switch (action)
    {
      case VG_STATUS_HANDLED:
      case VG_STATUS_FALSE_ALARM:
        /* 用户确认后停止升级；若已发出通知则同步一条最终状态 */

        vg_track_close(t, action);
        return 0;

      case VG_STATUS_SNOOZED:
        if (t->snooze_count >= cfg->snooze_max_count)
          {
            return -2;
          }

        t->snooze_count++;
        t->state = VG_STATE_SNOOZED;
        t->state_since_ms = now;
        t->snooze_deadline_ms =
            now + (uint64_t)cfg->snooze_minutes * 60 * 1000;
        t->evt.local_status = VG_STATUS_SNOOZED;
        vg_notify_changed(t);
        return 0;

      default:
        return -1;
    }
}

int vg_sm_active_count(void)
{
  int n = 0;
  int i;

  for (i = 0; i < VG_EVT_TYPE_MAX; i++)
    {
      if (g_tracks[i].active && g_tracks[i].materialized)
        {
          n++;
        }
    }

  return n;
}

static void vg_fill_view(const vg_track_t *t, vg_track_view_t *out)
{
  uint64_t now = vg_now_ms();

  memset(out, 0, sizeof(*out));
  out->evt = t->evt;
  out->state = t->state;
  out->snooze_count = t->snooze_count;
  out->notified = t->notified;
  out->local_only = t->local_only;

  if (t->state == VG_STATE_WARNING && t->deadline_ms > now)
    {
      out->countdown_sec = (uint32_t)((t->deadline_ms - now + 999) / 1000);
    }

  if (t->state == VG_STATE_SNOOZED && t->snooze_deadline_ms > now)
    {
      out->snooze_left_sec =
          (uint32_t)((t->snooze_deadline_ms - now + 999) / 1000);
    }
}

int vg_sm_get(int idx, vg_track_view_t *out)
{
  const vg_track_t *sorted[VG_EVT_TYPE_MAX];
  int n = 0;
  int i;
  int j;

  if (out == NULL || idx < 0)
    {
      return -1;
    }

  for (i = 0; i < VG_EVT_TYPE_MAX; i++)
    {
      if (g_tracks[i].active && g_tracks[i].materialized)
        {
          sorted[n++] = &g_tracks[i];
        }
    }

  if (idx >= n)
    {
      return -1;
    }

  /* 插入排序：按 PRD-03 展示优先级从高到低 */

  for (i = 1; i < n; i++)
    {
      const vg_track_t *key = sorted[i];

      for (j = i - 1;
           j >= 0 && vg_event_cmp_priority(&sorted[j]->evt, &key->evt) < 0;
           j--)
        {
          sorted[j + 1] = sorted[j];
        }

      sorted[j + 1] = key;
    }

  vg_fill_view(sorted[idx], out);
  return 0;
}

int vg_sm_top(vg_track_view_t *out)
{
  return vg_sm_get(0, out);
}

void vg_sm_inject(vg_event_type_t type, float confidence,
                  const char *phrase, const char *person,
                  float urgency, uint16_t repeat)
{
  uint64_t now = vg_now_ms();

  if (type == VG_EVT_DISTRESS_VOICE || type == VG_EVT_NAME_CALL_HELP)
    {
      vg_distress_obs_t obs;

      memset(&obs, 0, sizeof(obs));
      obs.ts_ms = now;
      obs.confidence = confidence;
      obs.urgency = urgency;
      obs.repeat_count = repeat;

      if (type == VG_EVT_DISTRESS_VOICE)
        {
          obs.kind = VG_VOICE_MOAN;
        }
      else
        {
          obs.kind = (phrase != NULL && phrase[0] != '\0' && urgency >= 0.5f)
                     ? VG_VOICE_HELP_PHRASE : VG_VOICE_NAME_CALL;
        }

      if (phrase != NULL)
        {
          vg_strlcpy(obs.matched_phrase, phrase, sizeof(obs.matched_phrase));
        }

      if (person != NULL)
        {
          vg_strlcpy(obs.person_label, person, sizeof(obs.person_label));
        }

      vg_sm_feed_distress(&obs);
    }
  else
    {
      vg_sound_obs_t obs;

      memset(&obs, 0, sizeof(obs));
      obs.ts_ms = now;
      obs.confidence = confidence;
      obs.window_ms = 1000;

      switch (type)
        {
          case VG_EVT_ALARM_BEEP:
            obs.cls = VG_SOUND_ALARM_BEEP;
            break;

          case VG_EVT_WATER_FLOW:
            obs.cls = VG_SOUND_WATER_FLOW;
            break;

          default:
            obs.cls = VG_SOUND_IMPACT;
            break;
        }

      vg_sm_feed_sound(&obs);
    }
}
