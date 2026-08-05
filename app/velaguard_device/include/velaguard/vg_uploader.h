/****************************************************************************
 * 安聆 VelaGuard - 事件上传与断网补发 (PRD-05)
 *
 * 只上传结构化摘要；网络中断不影响本地提醒，恢复后按 eventId 幂等补发。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_UPLOADER_H
#define __VELAGUARD_VG_UPLOADER_H

#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

int  vg_uploader_init(void);
void vg_uploader_deinit(void);

/* 入队一条待上传摘要（自动生成/复用 Agent 文案）。
 * 同一 eventId 重复入队时覆盖为最新状态，保证幂等。
 */

int vg_uploader_enqueue(const vg_safety_event_t *evt,
                        vg_upload_reason_t reason);

/* 同上，但使用调用方给定的文案（ai_agent / Skill 回灌）。
 * advice 为 NULL 或空串时退回自动生成。
 */

int vg_uploader_enqueue_advice(const vg_safety_event_t *evt,
                               vg_upload_reason_t reason,
                               const char *advice);

/* 周期驱动：到期重试。建议 1s 调用一次。 */

void vg_uploader_tick(void);

/* 待发送条数 */

int vg_uploader_pending(void);

/* 最近一次上传是否成功（用于 LCD 显示在线状态） */

bool vg_uploader_online(void);

/* 统计：成功 / 失败次数 */

void vg_uploader_stats(uint32_t *sent, uint32_t *failed);

uint32_t vg_uploader_dropped(void);
uint32_t vg_uploader_dead_letter(void);
uint32_t vg_uploader_storage_errors(void);
bool     vg_uploader_storage_saturated(void);

/* 强制立即尝试一次（演示用） */

void vg_uploader_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_UPLOADER_H */
