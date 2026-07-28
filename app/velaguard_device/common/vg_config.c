/****************************************************************************
 * 安聆 VelaGuard - 运行配置实现
 ****************************************************************************/

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "velaguard/vg_config.h"
#include "velaguard/vg_json.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_config_t g_config;
static bool        g_config_ready = false;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void vg_config_defaults(vg_config_t *cfg)
{
  const char *env;

  if (cfg == NULL)
    {
      return;
    }

  memset(cfg, 0, sizeof(*cfg));

  vg_strlcpy(cfg->device_id, "velaguard_demo_001", sizeof(cfg->device_id));
  vg_strlcpy(cfg->data_dir, VG_DEFAULT_DATA_DIR, sizeof(cfg->data_dir));
  vg_strlcpy(cfg->console_host, "192.168.43.1", sizeof(cfg->console_host));
  cfg->console_port = 8080;
  vg_strlcpy(cfg->console_path, "/events", sizeof(cfg->console_path));
  cfg->webhook_enabled = false;
  cfg->demo_mode = true;

  cfg->conf_alarm_beep = 0.80f;
  cfg->conf_water_flow = 0.70f;
  cfg->conf_impact     = 0.80f;
  cfg->conf_distress   = 0.70f;
  cfg->conf_name_call  = 0.70f;

  /* PRD-03 演示初始阈值 */

  cfg->alarm_hold_sec             = 5;
  cfg->water_notice_sec           = 30;
  cfg->water_warning_sec          = 120;
  cfg->warning_countdown_sec      = 60;
  cfg->impact_repeat_window_sec   = 60;
  cfg->distress_repeat_window_sec = 60;
  cfg->snooze_minutes             = 10;
  cfg->snooze_max_count           = 2;
  cfg->event_idle_timeout_sec     = 15;

  cfg->upload_retry_sec  = 10;
  cfg->upload_queue_max  = 32;

  /* 环境变量覆盖：便于主机侧回归与演示时临时改控制台地址，
   * 板端不设置这些变量时行为完全不变。
   */

  env = getenv("VELAGUARD_DATA_DIR");
  if (env != NULL && env[0] != '\0')
    {
      vg_strlcpy(cfg->data_dir, env, sizeof(cfg->data_dir));
    }

  env = getenv("VELAGUARD_CONSOLE_HOST");
  if (env != NULL && env[0] != '\0')
    {
      vg_strlcpy(cfg->console_host, env, sizeof(cfg->console_host));
    }

  env = getenv("VELAGUARD_CONSOLE_PORT");
  if (env != NULL && env[0] != '\0')
    {
      cfg->console_port = atoi(env);
    }

  env = getenv("VELAGUARD_DEVICE_ID");
  if (env != NULL && env[0] != '\0')
    {
      vg_strlcpy(cfg->device_id, env, sizeof(cfg->device_id));
    }
}

void vg_config_apply_demo(vg_config_t *cfg)
{
  if (cfg == NULL)
    {
      return;
    }

  /* 演示构建缩短阈值，正式部署由 config.json 覆盖为 3 分钟档 */

  cfg->demo_mode             = true;
  cfg->water_notice_sec      = 10;
  cfg->water_warning_sec     = 25;
  cfg->warning_countdown_sec = 15;
  cfg->snooze_minutes        = 1;
}

vg_config_t *vg_config(void)
{
  if (!g_config_ready)
    {
      vg_config_defaults(&g_config);
      g_config_ready = true;
    }

  return &g_config;
}

int vg_config_load(const char *path)
{
  vg_config_t *cfg = vg_config();
  char *buf;
  long size;
  FILE *fp;
  double d;
  int i;

  if (path == NULL)
    {
      return -1;
    }

  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return -1;
    }

  fseek(fp, 0, SEEK_END);
  size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  if (size <= 0 || size > 16384)
    {
      fclose(fp);
      return -1;
    }

  buf = malloc((size_t)size + 1);
  if (buf == NULL)
    {
      fclose(fp);
      return -1;
    }

  size = (long)fread(buf, 1, (size_t)size, fp);
  buf[size] = '\0';
  fclose(fp);

  vg_json_get_str(buf, "deviceId", cfg->device_id, sizeof(cfg->device_id));
  vg_json_get_str(buf, "dataDir", cfg->data_dir, sizeof(cfg->data_dir));
  vg_json_get_str(buf, "consoleHost", cfg->console_host,
                  sizeof(cfg->console_host));
  vg_json_get_str(buf, "consolePath", cfg->console_path,
                  sizeof(cfg->console_path));
  vg_json_get_str(buf, "webhookUrl", cfg->webhook_url,
                  sizeof(cfg->webhook_url));

  if (vg_json_get_int(buf, "consolePort", &i) == 0)
    {
      cfg->console_port = i;
    }

  vg_json_get_bool(buf, "webhookEnabled", &cfg->webhook_enabled);

  if (vg_json_get_bool(buf, "demoMode", &cfg->demo_mode) == 0 &&
      cfg->demo_mode)
    {
      vg_config_apply_demo(cfg);
    }

#define VG_CFG_F(key, field)                        \
  if (vg_json_get_double(buf, key, &d) == 0)        \
    {                                               \
      cfg->field = (float)d;                        \
    }

#define VG_CFG_U(key, field)                        \
  if (vg_json_get_int(buf, key, &i) == 0 && i >= 0) \
    {                                               \
      cfg->field = (uint32_t)i;                     \
    }

  VG_CFG_F("confAlarmBeep", conf_alarm_beep)
  VG_CFG_F("confWaterFlow", conf_water_flow)
  VG_CFG_F("confImpact", conf_impact)
  VG_CFG_F("confDistress", conf_distress)
  VG_CFG_F("confNameCall", conf_name_call)

  VG_CFG_U("alarmHoldSec", alarm_hold_sec)
  VG_CFG_U("waterNoticeSec", water_notice_sec)
  VG_CFG_U("waterWarningSec", water_warning_sec)
  VG_CFG_U("warningCountdownSec", warning_countdown_sec)
  VG_CFG_U("impactRepeatWindowSec", impact_repeat_window_sec)
  VG_CFG_U("distressRepeatWindowSec", distress_repeat_window_sec)
  VG_CFG_U("snoozeMinutes", snooze_minutes)
  VG_CFG_U("snoozeMaxCount", snooze_max_count)
  VG_CFG_U("eventIdleTimeoutSec", event_idle_timeout_sec)
  VG_CFG_U("uploadRetrySec", upload_retry_sec)
  VG_CFG_U("uploadQueueMax", upload_queue_max)

#undef VG_CFG_F
#undef VG_CFG_U

  free(buf);
  return 0;
}

void vg_config_dump(void)
{
  vg_config_t *cfg = vg_config();

  printf("VelaGuard 配置:\n");
  printf("  deviceId      : %s\n", cfg->device_id);
  printf("  dataDir       : %s\n", cfg->data_dir);
  printf("  console       : %s:%d%s\n",
         cfg->console_host, cfg->console_port, cfg->console_path);
  printf("  webhook       : %s\n",
         cfg->webhook_enabled ? "已启用(地址不回显)" : "未启用");
  printf("  demoMode      : %s\n", cfg->demo_mode ? "是" : "否");
  printf("  阈值(秒)      : 报警%" PRIu32 " 水流提醒%" PRIu32
         " 水流警告%" PRIu32 " 倒计时%" PRIu32 " snooze%" PRIu32 "分\n",
         cfg->alarm_hold_sec, cfg->water_notice_sec,
         cfg->water_warning_sec, cfg->warning_countdown_sec,
         cfg->snooze_minutes);
}
