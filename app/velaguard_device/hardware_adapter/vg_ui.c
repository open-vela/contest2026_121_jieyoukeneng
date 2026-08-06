/****************************************************************************
 * 安聆 VelaGuard - 本地 UI 实现 (PRD-04)
 ****************************************************************************/

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "velaguard/vg_capture.h"
#include "velaguard/vg_config.h"
#include "velaguard/vg_daemon.h"
#include "velaguard/vg_detector.h"
#include "velaguard/vg_diagnostics.h"
#include "velaguard/vg_enroll.h"
#include "velaguard/vg_event_log.h"
#include "velaguard/vg_event_sm.h"
#include "velaguard/vg_feature.h"
#include "velaguard/vg_indicator.h"
#include "velaguard/vg_time.h"
#include "velaguard/vg_ui.h"
#include "velaguard/vg_uploader.h"
#include "velaguard/vg_wifi.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_page_t         g_page = VG_PAGE_HOME;
static int               g_history_top;
static volatile vg_wizard_state_t g_wizard = VG_WIZ_IDLE;
static int               g_wiz_kind;
static int               g_wiz_phrase;
static int               g_wiz_person;
static int               g_wiz_style;
static volatile bool     g_wiz_restart_daemon;
static volatile bool     g_wiz_capture_running;
static volatile bool     g_wiz_prepare_running;
static volatile bool     g_wiz_cancel_requested;
static volatile bool     g_wiz_audio_ready = true;
static volatile bool     g_wiz_restore_running;

/* ASCII 备用名：串口调试和无中文字体的构建仍可使用。 */

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
  vg_safety_event_t last;
  size_t pos = 0;

  VG_APPEND("%s\n", ascii ? "== VelaGuard / HOME ==" : "== 安聆 VelaGuard · 监测首页 ==");

  VG_APPEND("%s: %s\n",
            ascii ? "Guard" : "守护",
            vg_capture_paused() ?
              (ascii ? "PAUSED" : vg_capture_pause_reason()) :
              (ascii ? "ON" : "守护中"));

  VG_APPEND("%s: %s\n", ascii ? "Time" : "现在时间",
            vg_time_reliable() ? (ascii ? "SYNCED" : "已同步")
                               : (ascii ? "NOT SYNCED" : "未同步"));

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

  VG_APPEND("%s\n", ascii ? "Device checks and enrollment are under TEST."
                            : "设备测试和家庭称呼录入请进入“测试”。");

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

static int vg_render_test(char *buf, size_t len, bool ascii)
{
  char ts[VG_TIMESTR_LEN];
  size_t pos = 0;

  vg_format_time(vg_wall_sec(), ts, sizeof(ts));
  VG_APPEND("%s\n", ascii ? "== TEST ==" : "== 测试与录入 ==");
  VG_APPEND("%s: %s\n", ascii ? "Time" : "时间",
            vg_time_reliable() ? ts : "未同步");
  VG_APPEND("%s: %s\n", ascii ? "Mic" : "麦克风",
            ascii ? "KEY1" : "按钮1");
  VG_APPEND("%s: %s\n", ascii ? "Play" : "播放",
            vg_indicator_has_audio_out() ? (ascii ? "READY" : "已发现")
                                         : (ascii ? "MISSING" : "未发现"));
  VG_APPEND("%s\n", ascii ? "KEY2 Play  KEY3 Enroll  KEY4 More"
                            : "2播放  3录入  4更多  5返回");
  if (vg_ui_wizard_active())
    {
      vg_tpl_kind_t kind = (vg_tpl_kind_t)g_wiz_kind;

      VG_APPEND("---- %s ----\n", ascii ? "ENROLLMENT" : "家庭称呼录入");
      switch (g_wizard)
        {
          case VG_WIZ_PICK_KIND:
            VG_APPEND("%s: %s\n", ascii ? "1 kind" : "1 类别",
                      g_wiz_kind == VG_TPL_NAME ? "姓名/称呼" :
                      g_wiz_kind == VG_TPL_HELP ? "求救短语" : "方言短语");
            break;
          case VG_WIZ_PICK_PHRASE:
            VG_APPEND("%s: %s\n", ascii ? "2 phrase" : "2 短语",
                      vg_enroll_preset_phrase(kind, g_wiz_phrase));
            break;
          case VG_WIZ_PICK_PERSON:
            VG_APPEND("%s: %s\n", ascii ? "3 person" : "3 成员",
                      vg_enroll_preset_person(g_wiz_person));
            break;
          case VG_WIZ_PICK_STYLE:
            VG_APPEND("%s: %s\n", ascii ? "4 style" : "4 语气",
                      g_wiz_style == VG_TPL_URGENT ? "急促" : "平静");
            break;
          case VG_WIZ_RECORDING:
            VG_APPEND("%s %d/%d%s\n", ascii ? "Say" : "请说",
                      vg_enroll_takes(), VG_ENROLL_MIN_TAKES,
                      g_wiz_capture_running ? "（采集中）" : "");
            break;
          case VG_WIZ_DONE:
            VG_APPEND("%s\n", ascii ? "Saved" : "模板已保存");
            break;
          default:
            break;
        }
    }
  if (vg_diagnostics_state() == VG_DIAG_RUNNING)
    {
      VG_APPEND("%s\n", ascii ? "Test is running..." : "测试正在进行，请稍候。");
    }
  else if (vg_diagnostics_state() == VG_DIAG_PASSED)
    {
      VG_APPEND("%s\n", ascii ? "Test passed" : "测试通过");
    }
  else if (vg_diagnostics_state() == VG_DIAG_FAILED)
    {
      VG_APPEND("%s\n", ascii ? "Test failed" : "测试失败，请查看设备状态");
    }
  return (int)pos;
}

static int vg_render_test_more(char *buf, size_t len, bool ascii)
{
  size_t pos = 0;

  VG_APPEND("%s\n", ascii ? "== TEST / MORE ==" : "== 更多测试 ==");
  VG_APPEND("%s: %s\n", ascii ? "Network" : "网络",
            vg_uploader_online() ? (ascii ? "ONLINE" : "已连接")
                                 : (ascii ? "OFFLINE" : "未连接"));
  VG_APPEND("%s\n", ascii ? "WiFi setup: open the setup page"
                            : "Wi-Fi配网：进入配网页面输入名称和密码");
  VG_APPEND("%s: %s:%d\n", ascii ? "Console" : "控制台",
            vg_config()->console_host, vg_config()->console_port);
  VG_APPEND("%s\n", ascii ? "button 1 setup; 2 network test; 3 phone bind"
                            : "按钮1配网；按钮2网络自检；按钮3手机绑定");
  VG_APPEND("%s\n", ascii ? "button 4 test page; 5 home"
                            : "按钮4测试页；按钮5主界面");
  return (int)pos;
}

static int vg_render_network(char *buf, size_t len, bool ascii)
{
  vg_wifi_status_t status;
  size_t pos = 0;

  vg_wifi_get_status(&status);
  VG_APPEND("%s\n", ascii ? "== Wi-Fi SETUP ==" : "== Wi-Fi 配网 ==");
  VG_APPEND("%s: %s\n", ascii ? "State" : "状态",
            vg_wifi_state_text(status.state, ascii));
  VG_APPEND("%s: %s\n", ascii ? "SSID" : "当前 Wi-Fi",
            status.ssid[0] != '\0' ? status.ssid : "-");
  VG_APPEND("%s: %s\n", ascii ? "IP" : "IP 地址",
            status.ip_ready ? status.ip : (ascii ? "NOT READY" : "未获取"));
  VG_APPEND("%s: %u\n", ascii ? "Networks" : "可选热点",
            status.network_count);

  if (status.error[0] != '\0')
    {
      VG_APPEND("%s: %s\n", ascii ? "Error" : "提示", status.error);
    }

  VG_APPEND("%s\n", ascii ? "Use buttons: scan, select, connect, bind, back"
                            : "按钮：扫描、选热点、连接、绑定、返回");
  return (int)pos;
}

static int vg_render_binding(char *buf, size_t len, bool ascii)
{
  const vg_config_t *cfg = vg_config();
  size_t pos = 0;
  const char *scheme = cfg->console_tls ? "https" : "http";

  VG_APPEND("%s\n", ascii ? "== PHONE BINDING =="
                            : "== 小米手机演示绑定 ==");
  VG_APPEND("%s: %s\n", ascii ? "Device" : "设备编号", cfg->device_id);
  VG_APPEND("%s: %s://%s:%d/\n", ascii ? "Console" : "控制台",
            scheme, cfg->console_host, cfg->console_port);
  if (cfg->demo_mode)
    {
      VG_APPEND("%s\n", ascii ? "Demo pairing key: see console terminal"
                                : "演示配对密钥：请查看控制台启动终端（每次启动随机生成）");
    }
  else
    {
      VG_APPEND("%s\n", ascii ? "Use the pairing key configured on console"
                                : "请使用控制台配置的配对密钥");
    }

  VG_APPEND("%s\n", ascii ? "1 Join the same Wi-Fi"
                            : "1 手机、开发板和电脑连接同一 Wi-Fi");
  VG_APPEND("%s\n", ascii ? "2 Open the console URL in Xiaomi browser"
                            : "2 小米浏览器打开上面的控制台地址");
  VG_APPEND("%s\n", ascii ? "3 Enter device ID and key, then pair"
                            : "3 输入设备编号和密钥，点击配对");
  VG_APPEND("%s\n", ascii ? "Serial: velaguard console set <PC-IP>"
                            : "串口可输入：velaguard console set <电脑IP>");
  VG_APPEND("%s\n", ascii ? "1 Wi-Fi  2 Test  4 Back  5 Home"
                            : "1网络设置  2测试页  4返回  5首页");
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
  switch (g_page)
    {
      case VG_PAGE_HOME:
        g_page = VG_PAGE_EVENT;
        break;

      case VG_PAGE_EVENT:
        g_page = VG_PAGE_HISTORY;
        break;

      case VG_PAGE_HISTORY:
        g_page = VG_PAGE_TEST;
        break;

      case VG_PAGE_TEST:
        g_page = VG_PAGE_TEST_MORE;
        break;

      default:
        g_page = VG_PAGE_HOME;
        break;
    }

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

      case VG_PAGE_TEST:
        return vg_render_test(buf, len, ascii);

      case VG_PAGE_TEST_MORE:
        return vg_render_test_more(buf, len, ascii);

      case VG_PAGE_NETWORK:
        return vg_render_network(buf, len, ascii);

      case VG_PAGE_BINDING:
        return vg_render_binding(buf, len, ascii);

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

static void vg_wizard_audio_restore(void);
static void vg_wizard_audio_restore_async(void);

static void *vg_wizard_prepare_worker(void *arg)
{
  int ret;

  (void)arg;
  ret = vg_daemon_stop();
  g_wiz_prepare_running = false;

  /* 取消可能发生在守护停止等待期间。由准备线程统一完成恢复，避免
   * 取消线程先启动守护、准备线程随后又把它停掉。 */
  if (g_wiz_cancel_requested)
    {
      vg_enroll_cancel();
      g_wizard = VG_WIZ_IDLE;
      g_wiz_cancel_requested = false;
      vg_wizard_audio_restore_async();
      vg_indicator_set_led(VG_LED_GUARD);
      return NULL;
    }

  g_wiz_audio_ready = ret == 0;
  if (ret < 0)
    {
      printf("[velaguard] 录入前守护停止失败，请先确认音频通路空闲\n");
    }
  return NULL;
}

static void *vg_wizard_restore_worker(void *arg)
{
  (void)arg;
  vg_wizard_audio_restore();
  g_wiz_restore_running = false;
  return NULL;
}

static void vg_wizard_audio_restore_async(void)
{
  pthread_t thread;

  if (!g_wiz_restart_daemon)
    {
      vg_capture_resume();
      g_wiz_audio_ready = true;
      return;
    }

  if (g_wiz_restore_running)
    {
      return;
    }

  g_wiz_audio_ready = false;
  g_wiz_restore_running = true;
  if (pthread_create(&thread, NULL, vg_wizard_restore_worker, NULL) != 0)
    {
      g_wiz_restore_running = false;
      printf("[velaguard] 守护恢复线程启动失败\n");
      return;
    }

  pthread_detach(thread);
}

static void *vg_wizard_capture_worker(void *arg)
{
  int16_t pcm[VG_WINDOW_SAMPLES];
  float feat[VG_FEATURE_DIM];
  int got = 0;
  int tries = 0;
  bool capture_failed = false;

  (void)arg;
  if (vg_capture_open(VG_SRC_MIC, NULL) < 0)
    {
      capture_failed = true;
      printf("[velaguard] 录入失败：无法打开麦克风\n");
      goto done;
    }

  while (!g_wiz_cancel_requested && got < VG_WINDOW_SAMPLES && tries++ < 120)
    {
      int n = vg_capture_read(pcm + got, VG_WINDOW_SAMPLES - got);

      if (n > 0)
        {
          got += n;
        }
      else
        {
          usleep(50000);
        }
    }

  vg_capture_close();
  if (!g_wiz_cancel_requested &&
      (got < VG_FRAME_LEN || vg_feature_extract(pcm, (size_t)got, feat) < 0))
    {
      capture_failed = true;
      printf("[velaguard] 录入失败：麦克风采样不足，请重试\n");
      goto done;
    }

  if (!g_wiz_cancel_requested && g_wizard == VG_WIZ_RECORDING &&
      vg_enroll_feed(feat) < 0)
    {
      capture_failed = true;
      printf("[velaguard] 录入失败：模板采样已达到上限\n");
    }

done:
  g_wiz_capture_running = false;
  if (g_wiz_cancel_requested)
    {
      vg_enroll_cancel();
      vg_wizard_audio_restore();
      vg_indicator_set_led(VG_LED_GUARD);
      g_wiz_cancel_requested = false;
    }
  else if (capture_failed)
    {
      /* 失败采样也要释放暂停状态，避免守护长期停在关闭状态。 */
      vg_wizard_audio_restore_async();
    }
  return NULL;
}

static int vg_wizard_capture_start(void)
{
  pthread_t thread;

  if (g_wiz_capture_running)
    {
      printf("[velaguard] 录入采样正在进行，请稍候\n");
      return -1;
    }

  if (!g_wiz_audio_ready)
    {
      printf("[velaguard] 音频通路正在切换，请稍候再采集\n");
      return -1;
    }

  g_wiz_capture_running = true;
  if (pthread_create(&thread, NULL, vg_wizard_capture_worker, NULL) != 0)
    {
      g_wiz_capture_running = false;
      printf("[velaguard] 录入采样线程启动失败\n");
      return -1;
    }

  pthread_detach(thread);
  printf("[velaguard] 已开始采集一段录入样本，请清晰说完选定短语\n");
  return 0;
}

static void vg_wizard_audio_restore(void)
{
  int ret;
  int retry;

  if (g_wiz_restart_daemon)
    {
      ret = -1;
      for (retry = 0; retry < 30; retry++)
        {
          ret = vg_daemon_start(VG_SRC_MIC, NULL);
          if (ret != -EBUSY)
            {
              break;
            }

          /* 停止超时或另一个清理者短暂占用生命周期锁时，等任务真正
           * 退出后再恢复，避免取消录入后永久停留在“音频切换中”。 */
          usleep(100 * 1000);
        }

      if (ret < 0)
        {
          g_wiz_audio_ready = false;
          printf("[velaguard] 守护恢复失败，请稍后重试\n");
          return;
        }

      g_wiz_audio_ready = true;
      g_wiz_restart_daemon = false;
    }
  else
    {
      vg_capture_resume();
      g_wiz_audio_ready = true;
    }
}

void vg_ui_wizard_start(void)
{
  if (g_wiz_prepare_running || g_wiz_restore_running ||
      g_wiz_capture_running)
    {
      printf("[velaguard] 音频通路正在切换，请稍候再进入录入\n");
      return;
    }

  g_page = VG_PAGE_TEST;
  g_wizard = VG_WIZ_PICK_KIND;
  g_wiz_kind = VG_TPL_NAME;
  g_wiz_phrase = 0;
  g_wiz_person = 0;
  g_wiz_style = VG_TPL_CALM;
  g_wiz_prepare_running = false;
  g_wiz_cancel_requested = false;
  g_wiz_restart_daemon = vg_daemon_running() &&
                         vg_capture_source() == VG_SRC_MIC;
  g_wiz_audio_ready = !vg_daemon_running();

  if (vg_daemon_running())
    {
      pthread_t thread;

      g_wiz_prepare_running = true;
      if (pthread_create(&thread, NULL, vg_wizard_prepare_worker, NULL) == 0)
        {
          pthread_detach(thread);
        }
      else
        {
          g_wiz_prepare_running = false;
          g_wiz_audio_ready = false;
          printf("[velaguard] 音频通路切换线程启动失败\n");
        }
    }

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

  if (g_wizard == VG_WIZ_RECORDING)
    {
      if (g_wiz_capture_running)
        {
          printf("[velaguard] 当前正在采集，请稍候再返回\n");
          return;
        }

      vg_enroll_cancel();
      g_wizard = VG_WIZ_PICK_STYLE;
      return;
    }

  if (g_wizard == VG_WIZ_DONE)
    {
      g_wizard = VG_WIZ_PICK_STYLE;
      return;
    }

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
        if (vg_enroll_takes() < VG_ENROLL_MIN_TAKES)
          {
            (void)vg_wizard_capture_start();
          }
        else if (!g_wiz_capture_running && vg_enroll_commit() >= 0)
          {
            g_wizard = VG_WIZ_DONE;
            vg_wizard_audio_restore_async();
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
  if (g_wiz_prepare_running)
    {
      g_wiz_cancel_requested = true;
      vg_enroll_cancel();
      g_wizard = VG_WIZ_IDLE;
      printf("[velaguard] 正在结束音频占用，取消后自动恢复守护\n");
      return;
    }

  if (g_wiz_capture_running)
    {
      g_wiz_cancel_requested = true;
      g_wizard = VG_WIZ_IDLE;
      printf("[velaguard] 已取消录入，当前采样结束后恢复守护\n");
      return;
    }

  vg_enroll_cancel();
  g_wizard = VG_WIZ_IDLE;
  vg_wizard_audio_restore_async();
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
