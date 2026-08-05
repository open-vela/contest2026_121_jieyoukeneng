/****************************************************************************
 * 安聆 VelaGuard - 运行配置实现
 ****************************************************************************/

#include <inttypes.h>
#include <math.h>
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

  cfg->schema_version = 1;
  cfg->config_revision = 0;
  vg_strlcpy(cfg->device_id, "velaguard_demo_001", sizeof(cfg->device_id));
  vg_strlcpy(cfg->data_dir, VG_DEFAULT_DATA_DIR, sizeof(cfg->data_dir));
  vg_strlcpy(cfg->console_host, "192.168.43.1", sizeof(cfg->console_host));
  cfg->console_port = 8080;
  vg_strlcpy(cfg->console_path, "/events", sizeof(cfg->console_path));
  vg_strlcpy(cfg->console_server_name, cfg->console_host,
             sizeof(cfg->console_server_name));
  cfg->console_tls = false;
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

  env = getenv("VELAGUARD_DEVICE_TOKEN");
  if (env != NULL && env[0] != '\0')
    {
      vg_strlcpy(cfg->device_token, env, sizeof(cfg->device_token));
    }

  env = getenv("VELAGUARD_PROFILE");
  if (env != NULL && strcmp(env, "production") == 0)
    {
      cfg->demo_mode = false;
    }

  env = getenv("VELAGUARD_CONSOLE_TLS");
  if (env != NULL && (strcmp(env, "1") == 0 ||
                      strcmp(env, "true") == 0))
    {
      cfg->console_tls = true;
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

static int vg_config_fail(char *reason, size_t reason_len,
                          const char *message)
{
  if (reason != NULL && reason_len > 0)
    {
      snprintf(reason, reason_len, "%s", message);
    }

  return -1;
}

static bool vg_config_id_valid(const char *value, size_t capacity)
{
  size_t i;
  size_t n;

  if (value == NULL || capacity == 0 ||
      memchr(value, '\0', capacity) == NULL)
    {
      return false;
    }

  n = strlen(value);
  if (n == 0 || n >= capacity)
    {
      return false;
    }

  for (i = 0; i < n; i++)
    {
      unsigned char c = (unsigned char)value[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
        {
          return false;
        }
    }

  return true;
}

int vg_config_validate(const vg_config_t *cfg, char *reason,
                       size_t reason_len)
{
  if (cfg == NULL)
    {
      return vg_config_fail(reason, reason_len, "配置为空");
    }

  if (cfg->schema_version != 1)
    {
      return vg_config_fail(reason, reason_len, "schemaVersion 不支持");
    }

  if (!vg_config_id_valid(cfg->device_id, sizeof(cfg->device_id)))
    {
      return vg_config_fail(reason, reason_len, "deviceId 非法");
    }

  if (cfg->data_dir[0] != '/' || cfg->console_path[0] != '/')
    {
      return vg_config_fail(reason, reason_len, "路径必须是绝对路径");
    }

  if (cfg->console_host[0] == '\0' || cfg->console_port <= 0 ||
      cfg->console_port > 65535)
    {
      return vg_config_fail(reason, reason_len, "控制台地址或端口非法");
    }

  if (cfg->console_tls && (cfg->console_ca_path[0] == '\0' ||
                           cfg->console_server_name[0] == '\0'))
    {
      return vg_config_fail(reason, reason_len,
                            "TLS 必须配置 CA 和服务端名称");
    }

  if (!cfg->demo_mode && !cfg->console_tls)
    {
      return vg_config_fail(reason, reason_len,
                            "生产 profile 禁止明文 HTTP");
    }

  if (!cfg->demo_mode && cfg->device_token[0] == '\0')
    {
      return vg_config_fail(reason, reason_len,
                            "生产 profile 缺少设备认证令牌");
    }

  if (!cfg->demo_mode && cfg->config_signature[0] == '\0')
    {
      return vg_config_fail(reason, reason_len,
                            "生产 profile 缺少配置签名");
    }

  if (!isfinite(cfg->conf_alarm_beep) || cfg->conf_alarm_beep < 0.0f ||
      cfg->conf_alarm_beep > 1.0f ||
      !isfinite(cfg->conf_water_flow) || cfg->conf_water_flow < 0.0f ||
      cfg->conf_water_flow > 1.0f ||
      !isfinite(cfg->conf_impact) || cfg->conf_impact < 0.0f ||
      cfg->conf_impact > 1.0f ||
      !isfinite(cfg->conf_distress) || cfg->conf_distress < 0.0f ||
      cfg->conf_distress > 1.0f ||
      !isfinite(cfg->conf_name_call) || cfg->conf_name_call < 0.0f ||
      cfg->conf_name_call > 1.0f)
    {
      return vg_config_fail(reason, reason_len, "置信度阈值必须在 0~1");
    }

  if (cfg->alarm_hold_sec == 0 || cfg->water_notice_sec == 0 ||
      cfg->water_warning_sec < cfg->water_notice_sec ||
      cfg->warning_countdown_sec == 0 ||
      cfg->event_idle_timeout_sec == 0 ||
      cfg->snooze_max_count > 10 || cfg->upload_retry_sec == 0 ||
      cfg->upload_retry_sec > 900 || cfg->upload_queue_max == 0 ||
      cfg->upload_queue_max > 64)
    {
      return vg_config_fail(reason, reason_len, "阈值或队列配额越界");
    }

  if (reason != NULL && reason_len > 0)
    {
      reason[0] = '\0';
    }
  return 0;
}

int vg_config_load(const char *path)
{
  vg_config_t *cfg = vg_config();
  vg_config_t candidate = *cfg;
  char *buf;
  long size;
  FILE *fp;
  double d;
  int i;
  int schema_version;
  int config_revision;

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

  if (vg_json_get_int(buf, "schemaVersion", &schema_version) == 0 &&
      schema_version >= 0)
    {
      candidate.schema_version = (uint32_t)schema_version;
    }
  if (vg_json_get_int(buf, "configRevision", &config_revision) == 0 &&
      config_revision >= 0)
    {
      candidate.config_revision = (uint32_t)config_revision;
    }
  vg_json_get_str(buf, "configSignature", candidate.config_signature,
                  sizeof(candidate.config_signature));
  vg_json_get_str(buf, "deviceId", candidate.device_id,
                  sizeof(candidate.device_id));
  vg_json_get_str(buf, "dataDir", candidate.data_dir,
                  sizeof(candidate.data_dir));
  vg_json_get_str(buf, "consoleHost", candidate.console_host,
                  sizeof(candidate.console_host));
  vg_json_get_str(buf, "consolePath", candidate.console_path,
                  sizeof(candidate.console_path));
  vg_json_get_str(buf, "consoleServerName", candidate.console_server_name,
                  sizeof(candidate.console_server_name));
  vg_json_get_str(buf, "consoleCaPath", candidate.console_ca_path,
                  sizeof(candidate.console_ca_path));
  vg_json_get_str(buf, "deviceToken", candidate.device_token,
                  sizeof(candidate.device_token));
  vg_json_get_str(buf, "webhookUrl", candidate.webhook_url,
                  sizeof(candidate.webhook_url));

  if (vg_json_get_int(buf, "consolePort", &i) == 0)
    {
      candidate.console_port = i;
    }

  vg_json_get_bool(buf, "consoleTls", &candidate.console_tls);
  vg_json_get_bool(buf, "webhookEnabled", &candidate.webhook_enabled);

  if (vg_json_get_bool(buf, "demoMode", &candidate.demo_mode) == 0 &&
      candidate.demo_mode)
    {
      vg_config_apply_demo(&candidate);
    }

#define VG_CFG_F(key, field)                        \
  if (vg_json_get_double(buf, key, &d) == 0)        \
    {                                               \
      candidate.field = (float)d;                   \
    }

#define VG_CFG_U(key, field)                        \
  if (vg_json_get_int(buf, key, &i) == 0 && i >= 0) \
    {                                               \
      candidate.field = (uint32_t)i;                \
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

  if (candidate.console_server_name[0] == '\0')
    {
      vg_strlcpy(candidate.console_server_name, candidate.console_host,
                 sizeof(candidate.console_server_name));
    }

  {
    char reason[96];
    if (vg_config_validate(&candidate, reason, sizeof(reason)) < 0)
      {
        fprintf(stderr, "[velaguard] 拒绝配置: %s\n", reason);
        return -2;
      }
  }

  *cfg = candidate;
  return 0;
}

void vg_config_dump(void)
{
  vg_config_t *cfg = vg_config();

  printf("VelaGuard 配置:\n");
  printf("  schema        : %u revision %" PRIu32 "\n",
         (unsigned)cfg->schema_version, cfg->config_revision);
  printf("  deviceId      : %s\n", cfg->device_id);
  printf("  dataDir       : %s\n", cfg->data_dir);
  printf("  console       : %s:%d%s\n",
         cfg->console_host, cfg->console_port, cfg->console_path);
  printf("  transport     : %s（服务端名称 %s）\n",
         cfg->console_tls ? "TLS（需生产 transport）" : "HTTP 演示 profile",
         cfg->console_server_name);
  printf("  webhook       : %s\n",
         cfg->webhook_enabled ? "已启用(地址不回显)" : "未启用");
  printf("  demoMode      : %s\n", cfg->demo_mode ? "是" : "否");
  printf("  阈值(秒)      : 报警%" PRIu32 " 水流提醒%" PRIu32
         " 水流警告%" PRIu32 " 倒计时%" PRIu32 " snooze%" PRIu32 "分\n",
         cfg->alarm_hold_sec, cfg->water_notice_sec,
         cfg->water_warning_sec, cfg->warning_countdown_sec,
         cfg->snooze_minutes);
}
