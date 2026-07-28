/****************************************************************************
 * 安聆 VelaGuard - 本地事件日志 (PRD-05)
 *
 * 限长 100 条，写满后环形覆盖最旧记录；关键记录写入即落盘，断电重启不丢失。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_EVENT_LOG_H
#define __VELAGUARD_VG_EVENT_LOG_H

#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 初始化并从磁盘恢复。dir 为 NULL 时使用配置中的 data_dir。 */

int vg_event_log_init(const char *dir);

void vg_event_log_deinit(void);

/* 追加一条事件；写满后覆盖最旧记录。成功返回 0。 */

int vg_event_log_append(const vg_safety_event_t *evt);

/* 按 eventId 更新已有记录（状态流转）。未找到返回 -1。 */

int vg_event_log_update(const vg_safety_event_t *evt);

/* 追加或更新：存在则更新，不存在则追加 */

int vg_event_log_put(const vg_safety_event_t *evt);

/* 当前记录条数（<= 100） */

int vg_event_log_count(void);

/* 累计写入条数（含被覆盖的） */

uint32_t vg_event_log_total(void);

/* 按"从新到旧"索引读取，idx = 0 为最新。成功返回 0。 */

int vg_event_log_get(int idx, vg_safety_event_t *out);

/* 按 eventId 查找。成功返回 0。 */

int vg_event_log_find(const char *event_id, vg_safety_event_t *out);

/* 清空日志（含磁盘文件） */

int vg_event_log_clear(void);

/* 导出最近 n 条为 JSON 数组，返回写入字节数 */

int vg_event_log_to_json(int n, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_EVENT_LOG_H */
