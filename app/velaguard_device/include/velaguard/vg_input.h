/****************************************************************************
 * 安聆 VelaGuard - 本地输入 (PRD-04)
 *
 * 主输入：板载 LRADC 按键（原理图 SW2—SW6，约 5 键）
 * 增强输入：电容触摸（若驱动可用，非依赖项）
 * 开发/演示：串口命令（与脚本注入共用入口）
 *
 * 三种输入统一映射到 vg_action_t，走完全相同的处理路径。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_INPUT_H
#define __VELAGUARD_VG_INPUT_H

#include <stdbool.h>

#include "velaguard/vg_diagnostics.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
  VG_ACT_NONE = 0,
  VG_ACT_PAGE,             /* SW2 翻页 / 录入向导中为"下一项" */
  VG_ACT_HANDLED,          /* SW3 已处理 */
  VG_ACT_FALSE_ALARM,      /* SW4 误报 */
  VG_ACT_SNOOZE,           /* SW5 稍后提醒 */
  VG_ACT_ENTER,            /* SW6 确认 / 进入录入向导 */
  VG_ACT_BACK,             /* 长按 SW6 取消 */
  VG_ACT_SCROLL_UP,
  VG_ACT_SCROLL_DOWN,
  VG_ACT_TEST_MIC,
  VG_ACT_TEST_SPEAKER,
  VG_ACT_TEST_NETWORK
} vg_action_t;

int  vg_input_init(void);
void vg_input_deinit(void);

/* 轮询硬件按键，返回本次读到的动作（无按键返回 VG_ACT_NONE） */

vg_action_t vg_input_poll(void);

/* 执行一个动作（按键与串口命令共用） */

void vg_input_dispatch(vg_action_t action);

/* 字符串 -> 动作，供串口命令使用 */

vg_action_t vg_input_parse(const char *s);

bool vg_input_has_buttons(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_INPUT_H */
