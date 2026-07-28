/****************************************************************************
 * 安聆 VelaGuard - 本地 UI 实现 (PRD-04)
 ****************************************************************************/

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "velaguard/vg_capture.h"
#include "velaguard/vg_config.h"
#include "velaguard/vg_detector.h"
#include "velaguard/vg_enroll.h"
#include "velaguard/vg_event_log.h"
#include "velaguard/vg_event_sm.h"
#include "velaguard/vg_indicator.h"
#include "velaguard/vg_time.h"
#include "velaguard/vg_ui.h"
#include "velaguard/vg_uploader.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_page_t         g_page = VG_PAGE_HOME;
static int               g_history_top;
static vg_wizard_state_t g_wizard = VG_WIZ_IDLE;
static int               g_wiz_kind;
static int               g_wiz_phrase;
static int               g_wiz_person;
static int               g_wiz_style;

/* ASCII 备用名（LVGL 默认字体不含 CJK） */

static const char * const g_type_ascii[VG_EVT_TYPE_MAX] =
{
  "ALARM BEEP", "DISTRESS VOICE", "WATER FLOW", "NAME/HELP CALL", "IMPACT"
};

static const char * const g_level_ascii[] =
{
  "NOTICE", "WARNING", "EMERGENCY"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static const char *vg_type_txt(vg_event_type_t t, bool ascii)
{
  if (t < 0 || t >= VG_EVT_TYPE_MAX)
    {
      return ascii ? "UNKNOWN" : "未知事件";
    }

  return ascii ? g_type_ascii[t] : vg_event_type_cn(t);
}

static const char *vg_level_txt(vg_level_t l, bool ascii)
{
  if (l < VG_LEVEL_NOTICE || l > VG_LEVEL_EMERGENCY)
    {
      return ascii ? "-" : "无";
    }

  return ascii ? g_level_ascii[l] : vg_level_cn(l);
}

static const char *vg_status_txt(vg_local_status_t s, bool ascii)
{
  return ascii ? vg_status_str(s) : vg_status_cn(s);
}

#define VG_APPEND(...)                                        \
  do                                                          \
    {                                                         \
      int _n = snprintf(buf + pos, len - pos, __VA_ARGS__);   \
      if (_n < 0 || (size_t)_n >= len - pos)                  \
        {                                                     \
          return (int)pos;                                    \
        }                                                     \
      pos += (size_t)_n;                                      \
    }                                                         \
  while (0)

static int vg_render_home(char *buf, size_t len, bool ascii)
{
  vg_detector_stats_t st;
  vg_safety_event_t last;
  size_t pos = 0;
  char why[64];

  vg_detector_stats(&st);

  VG_APPEND("%s\n", ascii ? "== VelaGuard / HOME ==" : "== 安聆 VelaGuard · 监测首页 ==");

  VG_APPEND("%s: %s | %s: %s\n",
            ascii ? "Guard" : "守护",
            vg_capture_paused() ?
              (ascii ? "PAUSED" : vg_capture_pause_reason()) :
              (ascii ? "ON" : "守护中"),
            ascii ? "Src" : "音源",
            ascii ? (vg_capture_source() == VG_SRC_MIC ? "MIC" :
                     vg_capture_source() == VG_SRC_WAV ? "WAV" : "SIM")
                  : vg_capture_source_name());

  VG_APPEND("%s: %s | %s: %s\n",
            ascii ? "Net" : "上传",
            vg_uploader_online() ? (ascii ? "ONLINE" : "在线")
                                 : (ascii ? "OFFLINE" : "离线"),
            ascii ? "Clock" : "时间",
            vg_time_reliable() ? (ascii ? "SYNCED" : "已同步")
                               : (ascii ? "NOT SYNCED" : "未同步"));

  if (!ascii)
    {
      VG_APPEND("夜间判定: %s\n", vg_night_reason());
    }

  VG_APPEND("%s: %d | %s: %d\n",
            ascii ? "Pending upload" : "待发送通知", vg_uploader_pending(),
            ascii ? "Active events" : "活跃事件", vg_sm_active_count());

  VG_APPEND("%s: %s\n",
            ascii ? "LED" : "指示灯",
            ascii ? (vg_indicator_led() == VG_LED_EMERGENCY ? "RED" :
                     vg_indicator_led() == VG_LED_WARNING ? "YELLOW" :
                     vg_indicator_led() == VG_LED_PAUSED ? "BLUE" : "GREEN")
                  : vg_indicator_led_text());

  VG_APPEND("%s: %" PRIu32 " | %s: %" PRIu32 "us\n",
            ascii ? "Windows" : "已处理窗口", st.windows,
            ascii ? "Avg" : "平均耗时", st.avg_us);

  if (vg_event_log_get(0, &last) == 0)
    {
      VG_APPEND("%s: [%s] %s %s\n",
                ascii ? "Last event" : "最近事件",
                vg_level_txt(last.level, ascii),
                vg_type_txt(last.type, ascii),
                vg_status_txt(last.local_status, ascii));
    }
  else
    {
      VG_APPEND("%s\n", ascii ? "Last event: none" : "最近事件: 暂无");
    }

  if (g_wizard != VG_WIZ_IDLE)
    {
      vg_tpl_kind_t kind = (vg_tpl_kind_t)g_wiz_kind;

      VG_APPEND("---- %s ----\n",
                ascii ? "ENROLL WIZARD" : "家庭称呼与求救词录入");

      switch (g_wizard)
        {
          case VG_WIZ_PICK_KIND:
            VG_APPEND("%s: %s\n",
                      ascii ? "1/4 Kind" : "1/4 选择类别",
                      g_wiz_kind == VG_TPL_NAME ?
                        (ascii ? "NAME" : "姓名/称呼") :
                      g_wiz_kind == VG_TPL_HELP ?
                        (ascii ? "HELP" : "求救短语") :
                        (ascii ? "DIALECT" : "方言短语"));
            break;

          case VG_WIZ_PICK_PHRASE:
            VG_APPEND("%s: %s\n",
                      ascii ? "2/4 Phrase" : "2/4 选择短语",
                      vg_enroll_preset_phrase(kind, g_wiz_phrase));
            break;

          case VG_WIZ_PICK_PERSON:
            VG_APPEND("%s: %s\n",
                      ascii ? "3/4 Person" : "3/4 选择成员",
                      vg_enroll_preset_person(g_wiz_person));
            break;

          case VG_WIZ_PICK_STYLE:
            VG_APPEND("%s: %s\n",
                      ascii ? "4/4 Style" : "4/4 表达方式",
                      g_wiz_style == VG_TPL_URGENT ?
                        (ascii ? "URGENT" : "急促") :
                        (ascii ? "CALM" : "平静"));
            break;

          case VG_WIZ_RECORDING:
            VG_APPEND("%s %d/%d\n",
                      ascii ? "Recording take" : "采集次数",
                      vg_enroll_takes(), VG_ENROLL_MIN_TAKES);
            VG_APPEND("%s\n",
                      ascii ? "Guard paused, only template stored"
                            : "守护已暂停；只保存声学模板，不保存录音");
            break;

          case VG_WIZ_DONE:
            VG_APPEND("%s\n", ascii ? "Saved" : "已保存模板");
            break;

          default:
            break;
        }
    }
  else
    {
      VG_APPEND("%s: %d %s\n",
                ascii ? "Templates" : "已录入模板", vg_enroll_count(),
                ascii ? "" : "条");

      if (!vg_enroll_min_set_ready(why, sizeof(why)) && !ascii)
        {
          VG_APPEND("最小集自检: %s\n", why);
        }

      VG_APPEND("[%s] %s\n",
                ascii ? "KEY1" : "按键1",
                ascii ? "Enter enroll wizard" : "进入录入向导");
    }

  return (int)pos;
}

static int vg_render_event(char *buf, size_t len, bool ascii)
{
  vg_track_view_t view;
  size_t pos = 0;
  int n;
  int i;

  VG_APPEND("%s\n", ascii ? "== VelaGuard / EVENT ==" : "== 安聆 VelaGuard · 事件确认 ==");

  if (vg_sm_top(&view) < 0)
    {
      VG_APPEND("%s\n", ascii ? "No active event. Guarding."
                              : "当前无待确认事件，守护中。");
      return (int)pos;
    }

  VG_APPEND("[%s] %s\n",
            vg_level_txt(view.evt.level, ascii),
            vg_type_txt(view.evt.type, ascii));
  VG_APPEND("ID %s  %s %.0f%%\n", view.evt.event_id,
            ascii ? "conf" : "置信度", (double)view.evt.confidence * 100.0);
  VG_APPEND("%s %" PRIu32 "s  %s %s\n",
            ascii ? "duration" : "持续",
            view.evt.duration_sec,
            ascii ? "status" : "状态",
            vg_status_txt(view.evt.local_status, ascii));

  if (view.evt.matched_phrase[0] != '\0' && !ascii)
    {
      VG_APPEND("匹配短语: %s (%s) 重复 %u 次\n",
                view.evt.matched_phrase, view.evt.person_label,
                (unsigned)view.evt.repeat_count);
    }

  if (view.countdown_sec > 0)
    {
      VG_APPEND("%s %" PRIu32 "s\n",
                ascii ? "Countdown" : "倒计时", view.countdown_sec);
    }

  if (view.snooze_left_sec > 0)
    {
      VG_APPEND("%s %" PRIu32 "s (%s %" PRIu32 "/%" PRIu32 ")\n",
                ascii ? "Snoozed" : "稍后提醒剩余", view.snooze_left_sec,
                ascii ? "used" : "已用", view.snooze_count,
                vg_config()->snooze_max_count);
    }

  if (view.local_only)
    {
      VG_APPEND("%s\n", ascii ? "Local confirm only, no remote push"
                              : "平静单次呼喊：仅本地确认，不远程通知");
    }

  if (!ascii)
    {
      VG_APPEND("摘要: %s\n", view.evt.summary);
    }

  VG_APPEND("[%s] %s  [%s] %s  [%s] %s\n",
            ascii ? "K2" : "按键2", ascii ? "Handled" : "已处理",
            ascii ? "K3" : "按键3", ascii ? "False alarm" : "误报",
            ascii ? "K4" : "按键4", ascii ? "Snooze" : "稍后提醒");

  n = vg_sm_active_count();
  if (n > 1)
    {
      VG_APPEND("%s %d\n", ascii ? "Queued events:" : "排队事件:", n - 1);

      for (i = 1; i < n; i++)
        {
          vg_track_view_t q;

          if (vg_sm_get(i, &q) < 0)
            {
              break;
            }

          VG_APPEND("  - [%s] %s %s\n",
                    vg_level_txt(q.evt.level, ascii),
                    vg_type_txt(q.evt.type, ascii),
                    vg_status_txt(q.evt.local_status, ascii));
        }
    }

  return (int)pos;
}

static int vg_render_history(char *buf, size_t len, bool ascii)
{
  size_t pos = 0;
  int total = vg_event_log_count();
  int i;

  VG_APPEND("%s (%d/%d)\n",
            ascii ? "== VelaGuard / HISTORY ==" : "== 安聆 VelaGuard · 历史记录 ==",
            total > 0 ? g_history_top + 1 : 0, total);

  if (total == 0)
    {
      VG_APPEND("%s\n", ascii ? "No records." : "暂无记录。");
      return (int)pos;
    }

  for (i = g_history_top; i < g_history_top + 6 && i < total; i++)
    {
      vg_safety_event_t evt;
      char ts[VG_TIMESTR_LEN];

      if (vg_event_log_get(i, &evt) < 0)
        {
          break;
        }

      vg_format_time(evt.started_at, ts, sizeof(ts));
      VG_APPEND("%s [%s] %s %s%s\n",
                ts + 5,           /* 省略年份，省屏幕宽度 */
                vg_level_txt(evt.level, ascii),
                vg_type_txt(evt.type, ascii),
                vg_status_txt(evt.local_status, ascii),
                evt.time_reliable ? "" : (ascii ? " (?)" : " (时间未同步)"));
    }

  return (int)pos;
}

#undef VG_APPEND

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_ui_init(void)
{
  g_page = VG_PAGE_HOME;
  g_history_top = 0;
  g_wizard = VG_WIZ_IDLE;
  return 0;
}

void vg_ui_deinit(void)
{
  g_wizard = VG_WIZ_IDLE;
}

void vg_ui_set_page(vg_page_t page)
{
  if (page >= 0 && page < VG_PAGE_MAX)
    {
      g_page = page;
      if (page == VG_PAGE_HISTORY)
        {
          g_history_top = 0;
        }
    }
}

vg_page_t vg_ui_page(void)
{
  return g_page;
}

void vg_ui_next_page(void)
{
  g_page = (vg_page_t)((g_page + 1) % VG_PAGE_MAX);
  if (g_page == VG_PAGE_HISTORY)
    {
      g_history_top = 0;
    }
}

void vg_ui_tick(void)
{
  /* 文本后端无需重绘；LVGL 后端在 vg_ui_lvgl.c 中挂接 */
}

void vg_ui_on_event(const vg_safety_event_t *evt, vg_state_t state)
{
  if (evt == NULL)
    {
      return;
    }

  if (state == VG_STATE_WARNING || state == VG_STATE_EMERGENCY)
    {
      vg_ui_set_page(VG_PAGE_EVENT);
      vg_indicator_backlight_wake();
    }
}

int vg_ui_render(char *buf, size_t len, bool ascii)
{
  if (buf == NULL || len < 64)
    {
      return -1;
    }

  buf[0] = '\0';

  switch (g_page)
    {
      case VG_PAGE_EVENT:
        return vg_render_event(buf, len, ascii);

      case VG_PAGE_HISTORY:
        return vg_render_history(buf, len, ascii);

      default:
        return vg_render_home(buf, len, ascii);
    }
}

void vg_ui_history_scroll(int delta)
{
  int total = vg_event_log_count();

  g_history_top += delta;
  if (g_history_top < 0)
    {
      g_history_top = 0;
    }

  if (g_history_top >= total)
    {
      g_history_top = total > 0 ? total - 1 : 0;
    }
}

/****************************************************************************
 * 录入向导 (PRD-02)
 ****************************************************************************/

void vg_ui_wizard_start(void)
{
  g_page = VG_PAGE_HOME;
  g_wizard = VG_WIZ_PICK_KIND;
  g_wiz_kind = VG_TPL_NAME;
  g_wiz_phrase = 0;
  g_wiz_person = 0;
  g_wiz_style = VG_TPL_CALM;

  /* 录入与守护采集互斥（PRD-01） */

  vg_capture_pause("守护已暂停：录入中");
  vg_indicator_set_led(VG_LED_PAUSED);
}

static int vg_wiz_option_count(void)
{
  switch (g_wizard)
    {
      case VG_WIZ_PICK_KIND:
        return VG_TPL_KIND_MAX;

      case VG_WIZ_PICK_PHRASE:
        return vg_enroll_preset_phrase_count((vg_tpl_kind_t)g_wiz_kind);

      case VG_WIZ_PICK_PERSON:
        return vg_enroll_preset_person_count();

      case VG_WIZ_PICK_STYLE:
        return 2;

      default:
        return 0;
    }
}

static int *vg_wiz_option_ref(void)
{
  switch (g_wizard)
    {
      case VG_WIZ_PICK_KIND:
        return &g_wiz_kind;

      case VG_WIZ_PICK_PHRASE:
        return &g_wiz_phrase;

      case VG_WIZ_PICK_PERSON:
        return &g_wiz_person;

      case VG_WIZ_PICK_STYLE:
        return &g_wiz_style;

      default:
        return NULL;
    }
}

void vg_ui_wizard_next(void)
{
  int n = vg_wiz_option_count();
  int *ref = vg_wiz_option_ref();

  if (ref != NULL && n > 0)
    {
      *ref = (*ref + 1) % n;

      if (g_wizard == VG_WIZ_PICK_KIND)
        {
          g_wiz_phrase = 0;
        }
    }
}

void vg_ui_wizard_prev(void)
{
  int n = vg_wiz_option_count();
  int *ref = vg_wiz_option_ref();

  if (ref != NULL && n > 0)
    {
      *ref = (*ref + n - 1) % n;

      if (g_wizard == VG_WIZ_PICK_KIND)
        {
          g_wiz_phrase = 0;
        }
    }
}

void vg_ui_wizard_confirm(void)
{
  switch (g_wizard)
    {
      case VG_WIZ_PICK_KIND:
        g_wizard = VG_WIZ_PICK_PHRASE;
        break;

      case VG_WIZ_PICK_PHRASE:
        g_wizard = VG_WIZ_PICK_PERSON;
        break;

      case VG_WIZ_PICK_PERSON:
        g_wizard = VG_WIZ_PICK_STYLE;
        break;

      case VG_WIZ_PICK_STYLE:
        vg_enroll_begin((vg_tpl_kind_t)g_wiz_kind,
                        (vg_tpl_style_t)g_wiz_style,
                        vg_enroll_preset_phrase((vg_tpl_kind_t)g_wiz_kind,
                                                g_wiz_phrase),
                        vg_enroll_preset_person(g_wiz_person));
        g_wizard = VG_WIZ_RECORDING;
        break;

      case VG_WIZ_RECORDING:
        if (vg_enroll_takes() >= VG_ENROLL_MIN_TAKES &&
            vg_enroll_commit() >= 0)
          {
            g_wizard = VG_WIZ_DONE;
          }
        break;

      case VG_WIZ_DONE:
      default:
        vg_ui_wizard_cancel();
        break;
    }
}

void vg_ui_wizard_cancel(void)
{
  vg_enroll_cancel();
  g_wizard = VG_WIZ_IDLE;
  vg_capture_resume();
  vg_indicator_set_led(VG_LED_GUARD);
}

vg_wizard_state_t vg_ui_wizard_state(void)
{
  return g_wizard;
}

bool vg_ui_wizard_active(void)
{
  return g_wizard != VG_WIZ_IDLE;
}
