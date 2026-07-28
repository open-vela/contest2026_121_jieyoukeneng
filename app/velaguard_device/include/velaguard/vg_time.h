/****************************************************************************
 * 安聆 VelaGuard - 时钟与夜间判定 (PRD-03)
 *
 * 板载 RTC 无备份电池，断电重启后时间不可信。未同步 NTP 时夜间加严规则
 * 自动失效；可选用光感低照度作为夜间旁证。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_TIME_H
#define __VELAGUARD_VG_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 单调时钟毫秒，用于所有超时与持续时间计算（不受 NTP 校时跳变影响） */

uint64_t vg_now_ms(void);

/* 虚拟时钟：离线 wav 回放与单元测试需要按窗口节奏推进时间，
 * 而不是等待真实时间流逝。启用后 vg_now_ms() 返回虚拟时间。
 */

void     vg_time_use_virtual(bool on);
bool     vg_time_is_virtual(void);
void     vg_time_virtual_set(uint64_t ms);
void     vg_time_virtual_advance(uint64_t ms);

/* 墙钟 Unix 秒，用于事件时间戳 */

int64_t vg_wall_sec(void);

/* 时间是否可信：系统时间已通过 NTP 同步（或被显式标记为可信） */

bool vg_time_reliable(void);

/* 标记时间已同步 / 失效。板端在 NTP 成功后调用。 */

void vg_time_set_reliable(bool reliable);

/* 启动时自检：系统时间早于 2025-01-01 视为不可信 */

void vg_time_init(void);

/* 光感旁证：持续低照度累计达到阈值时可恢复夜间加严规则。
 * lux < 0 表示无光感数据。
 */

void vg_time_feed_lux(float lux);

/* 当前是否判定为夜间（PRD-03）：
 *  - 时间可信：22:00 — 次日 07:00 为夜间；
 *  - 时间不可信：默认按日间处理，除非光感持续低照度 >= 10 分钟。
 */

bool vg_is_night(void);

/* 夜间判定依据说明，用于 LCD 与日志 */

const char *vg_night_reason(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_TIME_H */
