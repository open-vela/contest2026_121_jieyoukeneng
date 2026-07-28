/****************************************************************************
 * 安聆 VelaGuard - 本地提醒指示实现 (PRD-04)
 ****************************************************************************/

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "velaguard/vg_capture.h"
#include "velaguard/vg_indicator.h"
#include "velaguard/vg_time.h"

#ifndef CONFIG_VELAGUARD_LED_DEVICE
#  define CONFIG_VELAGUARD_LED_DEVICE "/dev/userleds"
#endif

#ifndef CONFIG_VELAGUARD_TONE_DIR
#  define CONFIG_VELAGUARD_TONE_DIR "/data/velaguard/tones"
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_led_mode_t g_mode = VG_LED_OFF;
static int           g_led_fd = -1;
static bool          g_has_audio_out;
static bool          g_full_duplex = false;   /* 实机标定前保守假设不支持 */
static uint64_t      g_last_blink_ms;
static bool          g_blink_on;
static char          g_led_text[48] = "LED 关闭";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* WS2812 单灯颜色：这里通过 userled 位掩码近似表达三色状态。
 * 实机接入真实 WS2812 驱动时只需替换本函数。
 */

static void vg_led_write(uint8_t r, uint8_t g, uint8_t b)
{
  if (g_led_fd >= 0)
    {
      uint8_t mask = (uint8_t)((r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0));

      if (write(g_led_fd, &mask, 1) != 1)
        {
          /* LED 写失败不影响告警主链路，静默忽略 */
        }
    }
}

static const char *vg_mode_name(vg_led_mode_t mode)
{
  switch (mode)
    {
      case VG_LED_GUARD:     return "绿色呼吸(守护中)";
      case VG_LED_PAUSED:    return "蓝色常亮(守护暂停)";
      case VG_LED_NOTICE:    return "绿色快闪(提醒)";
      case VG_LED_WARNING:   return "黄色闪烁(警告)";
      case VG_LED_EMERGENCY: return "红色急闪(紧急)";
      default:               return "LED 关闭";
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_indicator_init(void)
{
  struct stat st;

  g_led_fd = open(CONFIG_VELAGUARD_LED_DEVICE, O_WRONLY);
  if (g_led_fd < 0)
    {
      printf("[velaguard] 未检测到 LED 设备(%s)，降级为控制台指示\n",
             CONFIG_VELAGUARD_LED_DEVICE);
    }

  g_has_audio_out = (stat(CONFIG_VELAGUARD_TONE_DIR, &st) == 0);
  if (!g_has_audio_out)
    {
      printf("[velaguard] 未检测到提示音资源(%s)，降级为控制台指示\n",
             CONFIG_VELAGUARD_TONE_DIR);
    }

  vg_indicator_set_led(VG_LED_GUARD);
  return 0;
}

void vg_indicator_deinit(void)
{
  if (g_led_fd >= 0)
    {
      vg_led_write(0, 0, 0);
      close(g_led_fd);
      g_led_fd = -1;
    }
}

void vg_indicator_set_led(vg_led_mode_t mode)
{
  if (g_mode == mode)
    {
      return;
    }

  g_mode = mode;
  g_last_blink_ms = 0;
  g_blink_on = false;
  vg_strlcpy(g_led_text, vg_mode_name(mode), sizeof(g_led_text));
}

vg_led_mode_t vg_indicator_led(void)
{
  return g_mode;
}

void vg_indicator_play(vg_level_t level)
{
  const char *tone;
  uint32_t duration_ms;

  switch (level)
    {
      case VG_LEVEL_WARNING:
        tone = "warning.wav";
        duration_ms = 1200;
        break;

      case VG_LEVEL_EMERGENCY:
        tone = "emergency.wav";
        duration_ms = 1800;
        break;

      default:
        tone = "notice.wav";
        duration_ms = 400;
        break;
    }

  /* PRD-01/04：录放并发未验证通过时采用时分方案，
   * 播放期间暂停采集，单次中断上限 2 秒。
   */

  if (!g_full_duplex)
    {
      vg_capture_hold_for_playback(duration_ms);
    }

  if (g_has_audio_out)
    {
      char path[128];

      snprintf(path, sizeof(path), "%s/%s", CONFIG_VELAGUARD_TONE_DIR, tone);

      /* 交给官方 media 播放接口；这里保持与平台解耦，
       * 实机接入时替换为 media_uv_player / nxplayer 调用。
       */

      printf("[velaguard] 播放提示音 %s（%s）\n",
             path, g_full_duplex ? "录放并发" : "时分让路");
    }
  else
    {
      printf("[velaguard] 提示音(%s级) —— 无扬声器资源，控制台提示\n",
             vg_level_cn(level));
    }
}

void vg_indicator_backlight_wake(void)
{
  /* 实机接 LCD 背光 PWM；模拟器线仅记录 */

  printf("[velaguard] LCD 背光唤醒\n");
}

void vg_indicator_tick(void)
{
  uint64_t now = vg_now_ms();
  uint32_t period;

  switch (g_mode)
    {
      case VG_LED_EMERGENCY:
        period = 150;
        break;

      case VG_LED_WARNING:
        period = 400;
        break;

      case VG_LED_NOTICE:
        period = 800;
        break;

      case VG_LED_GUARD:
        period = 2000;
        break;

      default:
        period = 0;
        break;
    }

  if (period == 0)
    {
      vg_led_write(0, 0, 0);
      return;
    }

  if (now - g_last_blink_ms < period)
    {
      return;
    }

  g_last_blink_ms = now;
  g_blink_on = !g_blink_on;

  switch (g_mode)
    {
      case VG_LED_EMERGENCY:
        vg_led_write(g_blink_on ? 255 : 0, 0, 0);
        break;

      case VG_LED_WARNING:
        vg_led_write(g_blink_on ? 255 : 0, g_blink_on ? 200 : 0, 0);
        break;

      case VG_LED_PAUSED:
        vg_led_write(0, 0, 255);
        break;

      default:
        vg_led_write(0, g_blink_on ? 255 : 40, 0);
        break;
    }
}

const char *vg_indicator_led_text(void)
{
  return g_led_text;
}

bool vg_indicator_has_led(void)
{
  return g_led_fd >= 0;
}

bool vg_indicator_has_audio_out(void)
{
  return g_has_audio_out;
}

void vg_indicator_set_full_duplex(bool supported)
{
  g_full_duplex = supported;
  printf("[velaguard] 录放并发能力标定: %s\n",
         supported ? "支持全双工" : "不支持，采用时分方案(<=2s)");
}

bool vg_indicator_full_duplex(void)
{
  return g_full_duplex;
}
