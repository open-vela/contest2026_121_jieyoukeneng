/****************************************************************************
 * 安聆 VelaGuard - 运行配置 (PRD-03 阈值 / PRD-06 凭证管理)
 *
 * 真实配置存 /data/velaguard/config.json，不进 Git；仓库只提供
 * config.example.json 模板。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_CONFIG_H
#define __VELAGUARD_VG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define VG_URL_LEN   128
#define VG_PATH_LEN  96
#define VG_TOKEN_LEN 96
#define VG_SIGNATURE_LEN 128

#ifdef CONFIG_VELAGUARD_DATA_DIR
#  define VG_DEFAULT_DATA_DIR CONFIG_VELAGUARD_DATA_DIR
#else
#  define VG_DEFAULT_DATA_DIR "/data/velaguard"
#endif

typedef struct
{
  /* 配置协议与发布状态 */

  uint32_t schema_version;
  uint32_t config_revision;
  char     config_signature[VG_SIGNATURE_LEN];

  /* 身份与存储 */

  char     device_id[VG_DEVICE_ID_LEN];
  char     data_dir[VG_PATH_LEN];

  /* 通知主通道：局域网控制台 (PRD-06) */

  char     console_host[VG_URL_LEN];
  int      console_port;
  char     console_path[VG_PATH_LEN];
  bool     console_tls;
  char     console_server_name[VG_URL_LEN];
  char     console_ca_path[VG_PATH_LEN];
  char     device_cert_path[VG_PATH_LEN];
  char     device_key_path[VG_PATH_LEN];
  char     device_token[VG_TOKEN_LEN];

  /* 可选扩展：webhook 转发，失败不影响主通道 */

  char     webhook_url[VG_URL_LEN];
  bool     webhook_enabled;

  /* 演示构建：使用缩短阈值 (PRD-03) */

  bool     demo_mode;

  /* 识别置信度门限 */

  float    conf_alarm_beep;
  float    conf_water_flow;
  float    conf_impact;
  float    conf_distress;
  float    conf_name_call;

  /* 状态机阈值（秒） */

  uint32_t alarm_hold_sec;          /* 报警蜂鸣连续命中 -> 紧急 */
  uint32_t water_notice_sec;        /* 水流 -> 提醒 */
  uint32_t water_warning_sec;       /* 水流 -> 警告 */
  uint32_t warning_countdown_sec;   /* 警告级倒计时 -> 无人响应 */
  uint32_t impact_repeat_window_sec;/* 撞击重复窗口 */
  uint32_t distress_repeat_window_sec;
  uint32_t snooze_minutes;          /* 稍后提醒挂起时长 */
  uint32_t snooze_max_count;        /* 同一事件最多 snooze 次数 */
  uint32_t event_idle_timeout_sec;  /* 持续型事件多久无命中判定结束 */

  /* 上传重试 */

  uint32_t upload_retry_sec;
  uint32_t upload_queue_max;
} vg_config_t;

/* 取全局配置（首次调用返回内置默认值） */

vg_config_t *vg_config(void);

/* 载入默认值 */

void vg_config_defaults(vg_config_t *cfg);

/* 从 JSON 文件载入并覆盖默认值。文件不存在返回 -1（使用默认值继续运行）。 */

int vg_config_load(const char *path);

/* 对候选配置执行类型、范围和跨字段校验，不修改当前配置。 */

int vg_config_validate(const vg_config_t *cfg, char *reason, size_t reason_len);

/* 应用演示模式的缩短阈值（PRD-03：正式阈值 3 分钟，演示构建缩短） */

void vg_config_apply_demo(vg_config_t *cfg);

/* 打印当前配置（不含任何凭证明文） */

void vg_config_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_CONFIG_H */
