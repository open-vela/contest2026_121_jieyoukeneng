/****************************************************************************
 * 安聆 VelaGuard - 公共类型与结构化事件协议 (PRD-01/03/05)
 *
 * 本文件只依赖 C 标准库与 POSIX，可同时在 openvela/NuttX 与主机侧单元测试
 * 环境中编译。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_TYPES_H
#define __VELAGUARD_VG_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_EVENT_ID_LEN     24
#define VG_DEVICE_ID_LEN    32
#define VG_PHRASE_LEN       32
#define VG_LABEL_LEN        24
#define VG_SUMMARY_LEN      128
#define VG_TIMESTR_LEN      32

/* 本地事件日志容量：PRD-05 规定最多 100 条，写满环形覆盖 */

#define VG_EVENT_LOG_CAPACITY 100

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* 五类安全声景 eventType (PRD-05) */

typedef enum
{
  VG_EVT_ALARM_BEEP = 0,      /* 烟雾/燃气报警蜂鸣 */
  VG_EVT_DISTRESS_VOICE,      /* 呻吟/痛苦叫声/哭喊/连续呼救 */
  VG_EVT_WATER_FLOW,          /* 持续水流/疑似漏水 */
  VG_EVT_NAME_CALL_HELP,      /* 姓名/亲属称呼/方言求救词 */
  VG_EVT_IMPACT,              /* 玻璃破碎/重物撞击 */
  VG_EVT_TYPE_MAX
} vg_event_type_t;

/* 告警三级 (PRD-03) */

typedef enum
{
  VG_LEVEL_NONE = -1,
  VG_LEVEL_NOTICE = 0,
  VG_LEVEL_WARNING,
  VG_LEVEL_EMERGENCY
} vg_level_t;

/* 本地处理状态 (PRD-05) */

typedef enum
{
  VG_STATUS_PENDING = 0,
  VG_STATUS_HANDLED,
  VG_STATUS_FALSE_ALARM,
  VG_STATUS_SNOOZED,
  VG_STATUS_NO_RESPONSE
} vg_local_status_t;

/* 上传原因 (PRD-05) */

typedef enum
{
  VG_UPLOAD_MANUAL_TEST = 0,
  VG_UPLOAD_ESCALATED,
  VG_UPLOAD_SYNC
} vg_upload_reason_t;

/* 状态机内部状态 (PRD-03 状态集合) */

typedef enum
{
  VG_STATE_IDLE = 0,          /* 正常监测 */
  VG_STATE_SUSPECT,           /* 可疑：已有命中但未达提醒阈值 */
  VG_STATE_NOTICE,            /* 提醒 */
  VG_STATE_WARNING,           /* 警告：倒计时等待确认 */
  VG_STATE_EMERGENCY,         /* 紧急 */
  VG_STATE_SNOOZED,           /* 稍后提醒 */
  VG_STATE_CLOSED             /* 已处理/误报，事件结束 */
} vg_state_t;

/* 识别层声音类别 (PRD-01 环境声景通道) */

typedef enum
{
  VG_SOUND_OTHER = 0,         /* 背景噪声/普通说话（负样本） */
  VG_SOUND_ALARM_BEEP,
  VG_SOUND_WATER_FLOW,
  VG_SOUND_IMPACT,
  VG_SOUND_CLASS_MAX
} vg_sound_class_t;

/* 识别层人声异常类别 (PRD-01 人声通道 + PRD-02 模板通道) */

typedef enum
{
  VG_VOICE_NONE = 0,
  VG_VOICE_MOAN,              /* 呻吟 */
  VG_VOICE_SCREAM,            /* 痛苦叫声/哭喊 */
  VG_VOICE_SHOUT_HELP,        /* 连续呼救 */
  VG_VOICE_NAME_CALL,         /* 呼喊姓名/亲属称呼 */
  VG_VOICE_HELP_PHRASE,       /* 明确求救短语（含方言） */
  VG_VOICE_KIND_MAX
} vg_voice_kind_t;

/* PRD-01 输出接口：环境声景观测 */

typedef struct
{
  uint64_t         ts_ms;        /* 单调时钟毫秒 */
  vg_sound_class_t cls;
  float            confidence;   /* 0.0 ~ 1.0 */
  uint16_t         window_ms;    /* 观测窗口长度 */
} vg_sound_obs_t;

/* PRD-01 输出接口：人声异常观测
 *
 * 明确不含对话文本与原始音频，只有类别、模板标签与统计量。
 */

typedef struct
{
  uint64_t        ts_ms;
  vg_voice_kind_t kind;
  char            matched_phrase[VG_PHRASE_LEN];  /* 命中的本地模板短语 */
  char            person_label[VG_LABEL_LEN];     /* 预设标签，如"家庭成员 A" */
  float           confidence;
  float           urgency;       /* 急促程度 0.0(平静) ~ 1.0(急促) */
  uint16_t        repeat_count;  /* 观测窗口内重复次数 */
} vg_distress_obs_t;

/* PRD-05 结构化事件协议 */

typedef struct
{
  char               event_id[VG_EVENT_ID_LEN];
  char               device_id[VG_DEVICE_ID_LEN];
  vg_event_type_t    type;
  vg_level_t         level;
  float              confidence;
  int64_t            started_at;      /* Unix 秒（墙钟） */
  uint32_t           duration_sec;
  vg_local_status_t  local_status;
  vg_upload_reason_t upload_reason;
  bool               time_reliable;   /* 时间是否可信（NTP 已同步） */
  bool               night;           /* 事件发生时是否判定为夜间 */

  /* 人声事件可选附加字段 */

  char               matched_phrase[VG_PHRASE_LEN];
  char               person_label[VG_LABEL_LEN];
  float              urgency;
  uint16_t           repeat_count;

  char               summary[VG_SUMMARY_LEN];
} vg_safety_event_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* 安全字符串拷贝：总是以 '\0' 结尾，超长截断 */

void vg_strlcpy(char *dst, const char *src, size_t size);

/* 枚举 <-> 协议字符串 */

const char *vg_event_type_str(vg_event_type_t type);
const char *vg_level_str(vg_level_t level);
const char *vg_status_str(vg_local_status_t status);
const char *vg_upload_reason_str(vg_upload_reason_t reason);
const char *vg_state_str(vg_state_t state);
const char *vg_sound_class_str(vg_sound_class_t cls);
const char *vg_voice_kind_str(vg_voice_kind_t kind);

int vg_event_type_parse(const char *s, vg_event_type_t *out);
int vg_level_parse(const char *s, vg_level_t *out);
int vg_status_parse(const char *s, vg_local_status_t *out);
int vg_voice_kind_parse(const char *s, vg_voice_kind_t *out);

/* 中文显示名（LCD / 通知文案用） */

const char *vg_event_type_cn(vg_event_type_t type);
const char *vg_level_cn(vg_level_t level);
const char *vg_status_cn(vg_local_status_t status);

/* 事件优先级：数值越大越优先 (PRD-03 并发仲裁) */

int vg_event_priority(const vg_safety_event_t *evt);

/* 比较两个事件的展示优先级，返回 >0 表示 a 更优先 */

int vg_event_cmp_priority(const vg_safety_event_t *a,
                          const vg_safety_event_t *b);

/* 将 Unix 秒格式化为 ISO8601 (+08:00)，供协议字段使用 */

void vg_format_time(int64_t unix_sec, char *buf, size_t len);

/* 事件 -> JSON（结构化摘要，禁止包含原始音频/对话文本）
 * 返回写入的字节数（不含结尾 '\0'），失败返回负值。
 */

int vg_event_to_json(const vg_safety_event_t *evt, char *buf, size_t len);

/* JSON -> 事件（供控制台回填与单元测试用，容错的扁平解析） */

int vg_event_from_json(const char *json, vg_safety_event_t *evt);

/* 事件字段合法性校验，返回 0 合法，负值为第一个非法字段编号 */

int vg_event_validate(const vg_safety_event_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_TYPES_H */
