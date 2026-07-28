/****************************************************************************
 * 安聆 VelaGuard - 时钟与夜间判定实现 (PRD-03)
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "velaguard/vg_types.h"
#include "velaguard/vg_time.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_NIGHT_START_HOUR   22
#define VG_NIGHT_END_HOUR     7

/* 光感旁证：持续低照度 10 分钟 */

#define VG_LUX_DARK_THRESHOLD 5.0f
#define VG_LUX_HOLD_MS        (10 * 60 * 1000ULL)

/* 2025-01-01T00:00:00Z，早于此时间认为 RTC 未校准 */

#define VG_TIME_SANE_FLOOR    1735689600LL

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool     g_time_reliable = false;
static uint64_t g_dark_since_ms = 0;
static bool     g_dark_confirmed = false;
static char     g_night_reason[48] = "时间未同步，按日间标准";
static bool     g_virtual = false;
static uint64_t g_virtual_ms = 0;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

uint64_t vg_now_ms(void)
{
  struct timespec ts;

  if (g_virtual)
    {
      return g_virtual_ms;
    }

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

void vg_time_use_virtual(bool on)
{
  if (on && !g_virtual)
    {
      struct timespec ts;

      /* 从当前单调时间起步，避免时间倒退 */

      g_virtual_ms = (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
                     ? (uint64_t)ts.tv_sec * 1000ULL +
                       (uint64_t)(ts.tv_nsec / 1000000)
                     : 0;
    }

  g_virtual = on;
}

bool vg_time_is_virtual(void)
{
  return g_virtual;
}

void vg_time_virtual_set(uint64_t ms)
{
  g_virtual_ms = ms;
}

void vg_time_virtual_advance(uint64_t ms)
{
  g_virtual_ms += ms;
}

int64_t vg_wall_sec(void)
{
  return (int64_t)time(NULL);
}

bool vg_time_reliable(void)
{
  return g_time_reliable;
}

void vg_time_set_reliable(bool reliable)
{
  g_time_reliable = reliable;
}

void vg_time_init(void)
{
  g_time_reliable = (vg_wall_sec() >= VG_TIME_SANE_FLOOR);
  g_dark_since_ms = 0;
  g_dark_confirmed = false;
}

void vg_time_feed_lux(float lux)
{
  uint64_t now = vg_now_ms();

  if (lux < 0.0f)
    {
      /* 无光感数据：清除旁证 */

      g_dark_since_ms = 0;
      g_dark_confirmed = false;
      return;
    }

  if (lux <= VG_LUX_DARK_THRESHOLD)
    {
      if (g_dark_since_ms == 0)
        {
          g_dark_since_ms = now;
        }
      else if (now - g_dark_since_ms >= VG_LUX_HOLD_MS)
        {
          g_dark_confirmed = true;
        }
    }
  else
    {
      g_dark_since_ms = 0;
      g_dark_confirmed = false;
    }
}

bool vg_is_night(void)
{
  if (g_time_reliable)
    {
      time_t t = (time_t)(vg_wall_sec() + 8 * 3600);
      struct tm tm_buf;
      int hour;

      if (gmtime_r(&t, &tm_buf) == NULL)
        {
          vg_strlcpy(g_night_reason, "时钟读取失败，按日间标准", sizeof(g_night_reason));
          return false;
        }

      hour = tm_buf.tm_hour;
      if (hour >= VG_NIGHT_START_HOUR || hour < VG_NIGHT_END_HOUR)
        {
          snprintf(g_night_reason, sizeof(g_night_reason),
                   "时间已同步，当前 %02d 时属夜间", hour);
          return true;
        }

      snprintf(g_night_reason, sizeof(g_night_reason),
               "时间已同步，当前 %02d 时属日间", hour);
      return false;
    }

  if (g_dark_confirmed)
    {
      vg_strlcpy(g_night_reason, "时间未同步，光感持续低照度判为夜间", sizeof(g_night_reason));
      return true;
    }

  vg_strlcpy(g_night_reason, "时间未同步，夜间加严规则已降级", sizeof(g_night_reason));
  return false;
}

const char *vg_night_reason(void)
{
  return g_night_reason;
}
