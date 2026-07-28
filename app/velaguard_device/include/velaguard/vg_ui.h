/****************************************************************************
 * 安聆 VelaGuard - 本地 UI (PRD-04)
 *
 * 只做三个页面：监测首页 / 事件确认页 / 历史记录页；
 * 个性化录入作为首页内的向导状态，不新增页面。
 *
 * 两个后端共享同一份页面模型：
 *   - 控制台文本后端（始终可用，模拟器线与串口演示）
 *   - LVGL 后端（CONFIG_VELAGUARD_UI_LVGL，板载 LCD）
 ****************************************************************************/

#ifndef __VELAGUARD_VG_UI_H
#define __VELAGUARD_VG_UI_H

#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
  VG_PAGE_HOME = 0,     /* 监测首页 */
  VG_PAGE_EVENT,        /* 事件确认页 */
  VG_PAGE_HISTORY,      /* 历史记录页 */
  VG_PAGE_MAX
} vg_page_t;

/* 首页内的录入向导状态（PRD-02 入口） */

typedef enum
{
  VG_WIZ_IDLE = 0,
  VG_WIZ_PICK_KIND,
  VG_WIZ_PICK_PHRASE,
  VG_WIZ_PICK_PERSON,
  VG_WIZ_PICK_STYLE,
  VG_WIZ_RECORDING,
  VG_WIZ_DONE
} vg_wizard_state_t;

int  vg_ui_init(void);
void vg_ui_deinit(void);

void       vg_ui_set_page(vg_page_t page);
vg_page_t  vg_ui_page(void);
void       vg_ui_next_page(void);

/* 周期刷新（含 LVGL 后端重绘） */

void vg_ui_tick(void);

/* 事件变化通知：自动切到事件确认页并唤醒背光 */

void vg_ui_on_event(const vg_safety_event_t *evt, vg_state_t state);

/* 渲染当前页面为多行文本。ascii 为 true 时输出纯 ASCII，
 * 供未内置 CJK 字体的 LVGL 默认字体使用。
 */

int vg_ui_render(char *buf, size_t len, bool ascii);

/* 录入向导（板端无键盘，全部用按键在预设项之间切换） */

void              vg_ui_wizard_start(void);
void              vg_ui_wizard_next(void);
void              vg_ui_wizard_prev(void);
void              vg_ui_wizard_confirm(void);
void              vg_ui_wizard_cancel(void);
vg_wizard_state_t vg_ui_wizard_state(void);
bool              vg_ui_wizard_active(void);

/* 历史记录页翻页 */

void vg_ui_history_scroll(int delta);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_UI_H */
