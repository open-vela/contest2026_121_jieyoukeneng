/****************************************************************************
 * 安聆 VelaGuard - 板端诊断实现
 ****************************************************************************/

#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "velaguard/vg_capture.h"
#include "velaguard/vg_config.h"
#include "velaguard/vg_daemon.h"
#include "velaguard/vg_detector.h"
#include "velaguard/vg_diagnostics.h"
#include "velaguard/vg_feature.h"
#include "velaguard/vg_indicator.h"
#include "velaguard/vg_time.h"
#include "velaguard/vg_types.h"
#include "velaguard/vg_uploader.h"

static pthread_mutex_t g_diag_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_diag_running;

static void *vg_diag_worker(void *arg)
{
  vg_diag_kind_t kind = (vg_diag_kind_t)(uintptr_t)arg;

  (void)vg_diagnostics_run(kind);
  pthread_mutex_lock(&g_diag_lock);
  g_diag_running = false;
  pthread_mutex_unlock(&g_diag_lock);
  return NULL;
}

static int vg_diag_mic(void)
{
  static int16_t samples[VG_WINDOW_SAMPLES];
  int count;
  int i;
  int16_t min = 32767;
  int16_t max = -32768;
  uint64_t abs_sum = 0;
  bool restart_daemon = false;

  if (vg_daemon_running() && vg_capture_source() != VG_SRC_MIC)
    {
      printf("麦克风自检失败：当前音频源不是板载麦克风\n");
      return -1;
    }

  if (vg_daemon_running())
    {
      printf("麦克风自检：暂时暂停守护采集\n");
      restart_daemon = true;
      vg_daemon_stop();
    }

  if (vg_capture_open(VG_SRC_MIC, NULL) < 0)
    {
      printf("麦克风自检失败：无法打开采集设备\n");
      if (restart_daemon) (void)vg_daemon_start(VG_SRC_MIC, NULL);
      return -1;
    }

  count = vg_capture_read(samples, VG_WINDOW_SAMPLES);
  vg_capture_close();
  if (restart_daemon)
    {
      (void)vg_daemon_start(VG_SRC_MIC, NULL);
    }
  if (count <= 0)
    {
      if (restart_daemon) (void)vg_daemon_start(VG_SRC_MIC, NULL);
      printf("麦克风自检失败：没有读到采样数据\n");
      return -1;
    }

  for (i = 0; i < count; i++)
    {
      int value = samples[i];
      int absolute = value < 0 ? -value : value;

      if (samples[i] < min) min = samples[i];
      if (samples[i] > max) max = samples[i];
      abs_sum += (uint64_t)absolute;
    }

  printf("麦克风自检通过：%d 个采样，最小值 %d，最大值 %d，平均音量 %.2f\n",
         count, min, max, (double)abs_sum / (double)count);
  return 0;
}

static int vg_diag_network(void)
{
  vg_safety_event_t evt;
  bool online;

  memset(&evt, 0, sizeof(evt));
  snprintf(evt.event_id, sizeof(evt.event_id), "evt_network_test_%llu",
           (unsigned long long)vg_now_ms());
  vg_strlcpy(evt.device_id, vg_config()->device_id, sizeof(evt.device_id));
  evt.type = VG_EVT_IMPACT;
  evt.level = VG_LEVEL_NOTICE;
  evt.confidence = 1.0f;
  evt.started_at = vg_wall_sec();
  evt.local_status = VG_STATUS_HANDLED;
  evt.time_reliable = vg_time_reliable();
  evt.night = vg_is_night();
  snprintf(evt.summary, sizeof(evt.summary), "网络联通测试事件（非真实告警）");

  if (vg_uploader_enqueue(&evt, VG_UPLOAD_MANUAL_TEST) < 0)
    {
      printf("网络自检失败：测试事件无法入队\n");
      return -1;
    }

  vg_uploader_flush();
  online = vg_uploader_online();
  printf("网络自检：%s，待发送 %d 条\n",
         online ? "已连接控制台" : "未连接控制台（事件已本地保存）",
         vg_uploader_pending());
  return online ? 0 : -1;
}

int vg_diagnostics_run(vg_diag_kind_t kind)
{
  int failed = 0;

  if (kind == VG_DIAG_MIC || kind == VG_DIAG_ALL)
    failed += vg_diag_mic() != 0;

  if (kind == VG_DIAG_SPEAKER || kind == VG_DIAG_ALL)
    {
      if (!vg_indicator_has_audio_out())
        {
          printf("扬声器自检失败：未发现音频输出设备\n");
          failed++;
        }
      else
        {
          if (vg_indicator_play(VG_LEVEL_NOTICE) < 0)
            {
              printf("扬声器自检失败：播放请求未成功\n");
              failed++;
            }
          else
            {
              printf("扬声器自检通过：播放请求成功，请确认听到提示音\n");
            }
        }
    }

  if (kind == VG_DIAG_NETWORK || kind == VG_DIAG_ALL)
    failed += vg_diag_network() != 0;

  printf("板端自检%s\n", failed == 0 ? "通过" : "存在失败项");
  return failed == 0 ? 0 : -1;
}

int vg_diagnostics_start(vg_diag_kind_t kind)
{
  pthread_t thread;

  pthread_mutex_lock(&g_diag_lock);
  if (g_diag_running)
    {
      pthread_mutex_unlock(&g_diag_lock);
      printf("板端自检正在进行，请稍候\n");
      return -1;
    }

  g_diag_running = true;
  if (pthread_create(&thread, NULL, vg_diag_worker,
                     (void *)(uintptr_t)kind) != 0)
    {
      g_diag_running = false;
      pthread_mutex_unlock(&g_diag_lock);
      printf("板端自检启动失败\n");
      return -1;
    }
  pthread_detach(thread);
  pthread_mutex_unlock(&g_diag_lock);
  printf("板端自检已启动，请查看串口输出结果\n");
  return 0;
}

bool vg_diagnostics_running(void)
{
  bool running;

  pthread_mutex_lock(&g_diag_lock);
  running = g_diag_running;
  pthread_mutex_unlock(&g_diag_lock);
  return running;
}
