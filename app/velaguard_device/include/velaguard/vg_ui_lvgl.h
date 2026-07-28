/****************************************************************************
 * 安聆 VelaGuard - LVGL 显示后端接口 (PRD-04)
 ****************************************************************************/

#ifndef __VELAGUARD_VG_UI_LVGL_H
#define __VELAGUARD_VG_UI_LVGL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef CONFIG_VELAGUARD_UI_LVGL
int  vg_ui_lvgl_start(void);
void vg_ui_lvgl_stop(void);
bool vg_ui_lvgl_running(void);
#else
/* 未启用 LCD 界面时的空实现：velaguard ui 命令仍可查看同样的页面内容 */

static inline int  vg_ui_lvgl_start(void)   { return 0; }
static inline void vg_ui_lvgl_stop(void)    { }
static inline bool vg_ui_lvgl_running(void) { return false; }
#endif

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_UI_LVGL_H */
