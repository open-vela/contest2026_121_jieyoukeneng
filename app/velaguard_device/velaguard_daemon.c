/****************************************************************************
 * 安聆 VelaGuard - 守护主循环实现
 ****************************************************************************/

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#ifndef CONFIG_VELAGUARD_PRIORITY
#  define CONFIG_VELAGUARD_PRIORITY 100
#endif

#ifndef CONFIG_VELAGUARD_STACKSIZE
#  define CONFIG_VELAGUARD_STACKSIZE 16384
#endif

#include "velaguard/vg_agent.h"
#include "velaguard/vg_classifier.h"
#include "velaguard/vg_config.h"
#include "velaguard/vg_daemon.h"
#include "velaguard/vg_detector.h"
#include "velaguard/vg_enroll.h"
#include "velaguard/vg_event_log.h"
#include "velaguard/vg_event_sm.h"
#include "velaguard/vg_indicator.h"
#include "velaguard/vg_input.h"
#include "velaguard/vg_notifier.h"
#include "velaguard/vg_time.h"
#include "velaguard/vg_ui.h"
#include "velaguard/vg_ui_lvgl.h"
#include "velaguard/vg_uploader.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_TICK_MS 100

/****************************************************************************
 * Private Data
 ****************************************************************************/

static volatile bool g_running;
static bool          g_inited;

/* FLAT 构建下静态变量跨任务保留，但文件描述符属于任务。
 * 记录初始化时的 pid，换了任务就强制重新初始化，避免使用他人的句柄。
 */

static pid_t         g_owner_pid;
#ifndef __NuttX__
static pthread_t     g_thread;
#endif
static vg_level_t    g_last_level[VG_EVT_TYPE_MAX];
static uint64_t      g_last_upload_tick;

/****************************************************************************
 * Private Functions - 状态机回调
 ****************************************************************************/

static void vg_on_event_changed(const vg_safety_event_t *evt,
                                vg_state_t state, void *arg)
{
  vg_level_t prev;

  (void)arg;

  if (evt == NULL || evt->type < 0 || evt->type >= VG_EVT_TYPE_MAX)
    {
      return;
    }

  prev = g_last_level[evt->type];
  g_last_level[evt->type] = evt->level;

  printf("[velaguard] 事件 %s %s/%s 状态=%s %s\n",
         evt->event_id, vg_event_type_cn(evt->type),
         vg_level_cn(evt->level), vg_state_str(state), evt->summary);

  /* 分级提示音只在等级变化时播放，避免刷屏 */

  if (evt->level > prev)
    {
      vg_indicator_play(evt->level);
    }

  switch (evt->level)
    {
      case VG_LEVEL_EMERGENCY:
        vg_indicator_set_led(VG_LED_EMERGENCY);
        break;

      case VG_LEVEL_WARNING:
        vg_indicator_set_led(VG_LED_WARNING);
        break;

      default:
        vg_indicator_set_led(VG_LED_NOTICE);
        break;
    }

  vg_ui_on_event(evt, state);
}

static void vg_on_escalate(const vg_safety_event_t *evt, void *arg)
{
  (void)arg;

  printf("[velaguard] 无人确认，升级为紧急级并触发远程通知: %s\n",
         evt->event_id);

  /* 网络不可用时入队，恢复后按 eventId 幂等补发 */

  vg_uploader_enqueue(evt, VG_UPLOAD_ESCALATED);
}

static void vg_on_closed(const vg_safety_event_t *evt, void *arg)
{
  (void)arg;

  printf("[velaguard] 事件结束 %s -> %s\n",
         evt->event_id, vg_status_cn(evt->local_status));

  /* 已通知过的事件在用户确认后同步一条最终状态，家属侧卡片同步收敛 */

  if (evt->upload_reason == VG_UPLOAD_ESCALATED)
    {
      vg_uploader_enqueue(evt, VG_UPLOAD_SYNC);
    }

  g_last_level[evt->type] = VG_LEVEL_NONE;

  /* 没有活跃事件时回到守护绿 */

  if (vg_sm_active_count() == 0)
    {
      vg_indicator_set_led(vg_capture_paused() ?
                           VG_LED_PAUSED : VG_LED_GUARD);
    }
}

/****************************************************************************
 * Private Functions - 主循环
 ****************************************************************************/

static void *vg_daemon_thread(void *arg)
{
  (void)arg;

  printf("[velaguard] 守护任务已启动（音源: %s）\n",
         vg_capture_source_name());

  while (g_running)
    {
      vg_daemon_step();
      usleep(VG_TICK_MS * 1000);
    }

  printf("[velaguard] 守护任务已停止\n");
  return NULL;
}

#ifdef __NuttX__
static int vg_daemon_task(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  vg_daemon_thread(NULL);
  return 0;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_daemon_init(void)
{
  vg_config_t *cfg;
  char path[VG_PATH_LEN + 32];
  vg_sm_cb_t cb;
  bool first = !g_inited;
  int i;

  if (g_inited && g_owner_pid == getpid())
    {
      return 0;
    }

  g_owner_pid = getpid();
  cfg = vg_config();

  /* 真实配置不进 Git，缺省时使用内置默认值继续运行 */

  snprintf(path, sizeof(path), "%s/config.json", cfg->data_dir);
  if (vg_config_load(path) < 0)
    {
      printf("[velaguard] 未找到 %s，使用内置默认配置\n", path);
    }

  vg_time_init();
  vg_event_log_init(NULL);
  vg_enroll_init(NULL);
  vg_notifier_init();
  vg_agent_init();
  vg_uploader_init();
  vg_detector_init();
  vg_indicator_init();
  vg_input_init();
  vg_ui_init();

  memset(&cb, 0, sizeof(cb));
  cb.on_event_changed = vg_on_event_changed;
  cb.on_escalate = vg_on_escalate;
  cb.on_closed = vg_on_closed;

  if (first)
    {
      /* 冷启动：清空状态机 */

      vg_sm_init(&cb);

      for (i = 0; i < VG_EVT_TYPE_MAX; i++)
        {
          g_last_level[i] = VG_LEVEL_NONE;
        }
    }
  else
    {
      /* 换了任务（新的 NSH 命令）：只接管回调，保留已有事件轨道，
       * 保证 `velaguard sim` 之后 `velaguard ack` 能找到同一个事件。
       */

      vg_sm_set_callbacks(&cb);
    }

  g_inited = true;
  return 0;
}

void vg_daemon_deinit(void)
{
  if (!g_inited)
    {
      return;
    }

  vg_daemon_stop();
  vg_ui_lvgl_stop();
  vg_input_deinit();
  vg_indicator_deinit();
  vg_detector_deinit();
  vg_capture_close();
  vg_uploader_deinit();
  vg_enroll_deinit();
  vg_event_log_deinit();
  vg_ui_deinit();
  g_inited = false;
}

void vg_daemon_step(void)
{
  vg_action_t act;
  uint64_t now;

  /* 1. 识别：拉取音频窗口并产生观测 */

  vg_detector_pump();

  /* 2. 决策：倒计时、snooze、升级 */

  vg_sm_tick();

  /* 3. 本地交互：按键与指示 */

  act = vg_input_poll();
  if (act != VG_ACT_NONE)
    {
      vg_input_dispatch(act);
    }

  vg_indicator_tick();
  vg_ui_tick();

  /* 4. 上传重试（按配置节流，断网期间不空转） */

  now = vg_now_ms();
  if (now - g_last_upload_tick >= 1000)
    {
      g_last_upload_tick = now;
      vg_uploader_tick();
    }
}

int vg_daemon_start(vg_source_t src, const char *path)
{
  if (g_running)
    {
      printf("[velaguard] 守护任务已在运行\n");
      return 0;
    }

  vg_daemon_init();

  if (src != VG_SRC_NONE && vg_capture_open(src, path) < 0)
    {
      printf("[velaguard] 音频源不可用，转为模拟注入模式"
             "（velaguard sim ...）\n");
      vg_capture_open(VG_SRC_NONE, NULL);
    }

  vg_indicator_set_led(VG_LED_GUARD);
  vg_ui_lvgl_start();

  g_running = true;

#ifdef __NuttX__
  /* NSH 内建命令返回后其 pthread 会随任务组销毁，
   * 因此守护必须作为独立任务存在。
   */

  if (task_create("velaguard_d", CONFIG_VELAGUARD_PRIORITY,
                  CONFIG_VELAGUARD_STACKSIZE, vg_daemon_task, NULL) < 0)
    {
      g_running = false;
      printf("[velaguard] 守护任务创建失败\n");
      return -1;
    }
#else
  if (pthread_create(&g_thread, NULL, vg_daemon_thread, NULL) != 0)
    {
      g_running = false;
      printf("[velaguard] 守护任务创建失败\n");
      return -1;
    }

  pthread_detach(g_thread);
#endif

  return 0;
}

int vg_daemon_stop(void)
{
  if (!g_running)
    {
      return 0;
    }

  g_running = false;
  usleep(VG_TICK_MS * 2000);
  return 0;
}

bool vg_daemon_running(void)
{
  return g_running;
}

void vg_daemon_status(void)
{
  vg_detector_stats_t st;
  vg_config_t *cfg = vg_config();
  uint32_t agent_ok;
  uint32_t fallback;
  uint32_t sent;
  uint32_t failed;
  char why[64];

  vg_detector_stats(&st);
  vg_notifier_stats(&agent_ok, &fallback);
  vg_uploader_stats(&sent, &failed);

  printf("==== 安聆 VelaGuard 运行状态 ====\n");
  printf("守护任务   : %s\n", g_running ? "运行中" : "已停止");
  printf("采集状态   : %s（音源: %s）\n",
         vg_capture_paused() ? vg_capture_pause_reason() : "守护中",
         vg_capture_source_name());
  printf("录放并发   : %s\n",
         vg_indicator_full_duplex() ? "支持全双工" : "时分方案(<=2s)");
  printf("时间       : %s / %s\n",
         vg_time_reliable() ? "已同步" : "未同步（夜间规则已降级）",
         vg_night_reason());
  printf("模型       : %s\n", vg_classifier_info());
  printf("识别统计   : 窗口 %" PRIu32 " 环境命中 %" PRIu32
         " 人声命中 %" PRIu32 " 模板命中 %" PRIu32 "\n",
         st.windows, st.sound_hits, st.voice_hits, st.template_hits);
  printf("单窗口耗时 : 平均 %" PRIu32 "us / 峰值 %" PRIu32 "us"
         "（预算 < 500000us）\n", st.avg_us, st.max_us);
  printf("活跃事件   : %d  日志 %d/%d 条（累计 %" PRIu32 "）\n",
         vg_sm_active_count(), vg_event_log_count(),
         VG_EVENT_LOG_CAPACITY, vg_event_log_total());
  printf("通知文案   : 来源 %s（Agent %" PRIu32 " 次 / 模板兜底 %" PRIu32
         " 次）\n", vg_agent_source(), agent_ok, fallback);
  printf("上传       : %s 控制台 %s:%d%s 成功 %" PRIu32 " 失败 %" PRIu32
         " 待发 %d\n",
         vg_uploader_online() ? "在线" : "离线",
         cfg->console_host, cfg->console_port, cfg->console_path,
         sent, failed, vg_uploader_pending());
  vg_enroll_min_set_ready(why, sizeof(why));
  printf("录入模板   : %d 条  %s\n", vg_enroll_count(), why);
  printf("硬件       : LED %s / 扬声器 %s / 按键 %s\n",
         vg_indicator_has_led() ? "可用" : "不可用",
         vg_indicator_has_audio_out() ? "可用" : "不可用",
         vg_input_has_buttons() ? "可用" : "不可用");
}
