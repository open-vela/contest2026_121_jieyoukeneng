/****************************************************************************
 * 安聆 VelaGuard - 事件状态机与告警规则 (PRD-03)
 *
 * 识别层只给出类别与置信度，是否提醒/升级/通知全部由本模块的可解释规则决定。
 * 本模块不依赖任何硬件，可在模拟器与主机侧单元测试中独立运行。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_EVENT_SM_H
#define __VELAGUARD_VG_EVENT_SM_H

#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 状态机对外回调：由 main/daemon 装配 UI、日志与通知 */

typedef struct
{
  /* 事件首次产生或状态/等级发生变化 */

  void (*on_event_changed)(const vg_safety_event_t *evt,
                           vg_state_t state, void *arg);

  /* 升级为紧急级、需要远程通知（PRD-06 触发条件） */

  void (*on_escalate)(const vg_safety_event_t *evt, void *arg);

  /* 事件结束（已处理/误报/自动关闭） */

  void (*on_closed)(const vg_safety_event_t *evt, void *arg);

  void *arg;
} vg_sm_cb_t;

/* 单个事件轨道的对外快照 */

typedef struct
{
  vg_safety_event_t evt;
  vg_state_t        state;
  uint32_t          countdown_sec;   /* 警告倒计时剩余秒，0 表示无倒计时 */
  uint32_t          snooze_left_sec; /* snooze 剩余秒 */
  uint32_t          snooze_count;
  bool              notified;
  bool              local_only;      /* 仅本地确认，不升级远程通知 */
} vg_track_view_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void vg_sm_init(const vg_sm_cb_t *cb);
void vg_sm_reset(void);

/* 只更新回调、不清空事件轨道。
 *
 * FLAT 构建下每条 NSH 命令都是新任务，但静态数据跨任务保留：
 * `velaguard sim` 产生的事件必须能被后续 `velaguard ack` 看到，
 * 因此新任务接管时只重新登记回调，绝不能重置状态机。
 */

void vg_sm_set_callbacks(const vg_sm_cb_t *cb);

/* 识别层输入 (PRD-01) */

void vg_sm_feed_sound(const vg_sound_obs_t *obs);
void vg_sm_feed_distress(const vg_distress_obs_t *obs);

/* 周期驱动：倒计时、snooze 到期、持续事件结束判定。建议 200ms ~ 1s 调用。 */

void vg_sm_tick(void);

/* 本地确认操作 (PRD-04)：handled / false_alarm / snoozed
 * event_id 为 NULL 时作用于当前最高优先级事件。
 * 返回 0 成功，-1 未找到事件，-2 snooze 次数已用尽。
 */

int vg_sm_ack(const char *event_id, vg_local_status_t action);

/* 当前活跃事件数量（未结束） */

int vg_sm_active_count(void);

/* 将锁内复制的事件快照在锁外分发给 UI、日志和上传适配器。守护任务可在
 * feed/tick 后显式调用；当前实现也会在各公共输入函数返回前自动分发。 */

void vg_sm_dispatch(void);

/* 通知队列溢出计数，供 status/metrics 读取。 */

uint32_t vg_sm_notify_dropped(void);

/* 取最高优先级的活跃事件（PRD-03 并发仲裁）。无活跃事件返回 -1。 */

int vg_sm_top(vg_track_view_t *out);

/* 遍历活跃事件，按优先级从高到低。idx 从 0 开始。 */

int vg_sm_get(int idx, vg_track_view_t *out);

/* 供演示/测试使用：直接注入一个成型的观测（等价于识别层输出） */

void vg_sm_inject(vg_event_type_t type, float confidence,
                  const char *phrase, const char *person,
                  float urgency, uint16_t repeat);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_EVENT_SM_H */
