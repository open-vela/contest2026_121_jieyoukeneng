/****************************************************************************
 * 安聆 VelaGuard - LVGL 显示后端 (PRD-04)
 *
 * 与控制台后端共用同一份页面模型（vg_ui.c），这里只负责把渲染结果
 * 画到 LCD 上，并提供分级配色的状态条。
 *
 * 注意：LVGL 默认字体不含 CJK 字形，因此 LCD 上使用 ASCII 文案
 *（vg_ui_render(..., ascii=true)）；控制台与远程通知使用中文文案。
 * 若后续内置 CJK 子集字体，只需把 ascii 改为 false。
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_VELAGUARD_UI_LVGL

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

#include "velaguard/vg_event_sm.h"
#include "velaguard/vg_ui.h"
#include "velaguard/vg_ui_lvgl.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_t         g_thread;
static volatile bool     g_running;
static lv_obj_t         *g_label;
static lv_obj_t         *g_bar;
static lv_nuttx_result_t g_result;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t vg_level_color(void)
{
  vg_track_view_t view;

  if (vg_sm_top(&view) < 0)
    {
      return 0x1f7a3d;      /* 守护绿 */
    }

  switch (view.evt.level)
    {
      case VG_LEVEL_EMERGENCY:
        return 0xc62828;    /* 紧急红 */

      case VG_LEVEL_WARNING:
        return 0xf9a825;    /* 警告黄 */

      default:
        return 0x2e7d32;    /* 提醒绿 */
    }
}

static void vg_lvgl_refresh(lv_timer_t *timer)
{
  static char text[1024];

  (void)timer;

  if (vg_ui_render(text, sizeof(text), true) > 0)
    {
      lv_label_set_text(g_label, text);
    }

  lv_obj_set_style_bg_color(g_bar, lv_color_hex(vg_level_color()), 0);
}

static void *vg_lvgl_thread(void *arg)
{
  lv_nuttx_dsc_t info;

  (void)arg;

  lv_init();
  lv_nuttx_dsc_init(&info);
#ifdef CONFIG_LV_USE_NUTTX_LCD
  /* Gemini-S1 exposes the ILI9341 through NuttX's LCD device. */
  info.fb_path = "/dev/lcd0";
#endif

  lv_nuttx_init(&info, &g_result);

  /* 必要延时，影响初始化顺序（Gemini-S1 板级踩坑，无此延时 display 不上屏） */

  usleep(100000);

  if (g_result.disp == NULL)
    {
      printf("[velaguard] LVGL 显示初始化失败，UI 降级为控制台输出\n");
      g_running = false;
      return NULL;
    }

  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x10141c), 0);

  g_bar = lv_obj_create(lv_screen_active());
  lv_obj_set_size(g_bar, LV_PCT(100), 8);
  lv_obj_align(g_bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_border_width(g_bar, 0, 0);
  lv_obj_set_style_radius(g_bar, 0, 0);

  g_label = lv_label_create(lv_screen_active());
  lv_obj_set_width(g_label, LV_PCT(96));
  lv_obj_align(g_label, LV_ALIGN_TOP_LEFT, 6, 16);
  lv_label_set_long_mode(g_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(g_label, lv_color_hex(0xe8eaf0), 0);
  lv_label_set_text(g_label, "VelaGuard starting...");

  lv_timer_create(vg_lvgl_refresh, 500, NULL);

  while (g_running)
    {
      uint32_t idle = lv_timer_handler();

      if (idle == LV_NO_TIMER_READY || idle > 50)
        {
          idle = 50;
        }

      usleep(idle * 1000);
    }

  lv_nuttx_deinit(&g_result);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_ui_lvgl_start(void)
{
  pthread_attr_t attr;

  if (g_running)
    {
      return 0;
    }

  g_running = true;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_VELAGUARD_UI_STACKSIZE);
  if (pthread_create(&g_thread, &attr, vg_lvgl_thread, NULL) != 0)
    {
      g_running = false;
      pthread_attr_destroy(&attr);
      return -1;
    }

  pthread_attr_destroy(&attr);

  return 0;
}

void vg_ui_lvgl_stop(void)
{
  if (!g_running)
    {
      return;
    }

  g_running = false;
  pthread_join(g_thread, NULL);
}

bool vg_ui_lvgl_running(void)
{
  return g_running;
}

#endif /* CONFIG_VELAGUARD_UI_LVGL */
