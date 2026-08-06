/****************************************************************************
 * 安聆 VelaGuard - LVGL 显示后端 (PRD-04)
 *
 * 与控制台后端共用同一份页面模型（vg_ui.c），这里只负责把渲染结果
 * 画到 LCD 上，并提供分级配色的状态条。
 *
 * Gemini-S1 固件内置精简中文字体，LCD 与控制台共用中文页面模型。
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_VELAGUARD_UI_LVGL

#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __NuttX__
#  include <nuttx/sched.h>
#endif

#include <lvgl/lvgl.h>

#include "velaguard/vg_event_sm.h"
#include "velaguard/vg_enroll.h"
#include "velaguard/vg_input.h"
#include "velaguard/vg_time.h"
#include "velaguard/vg_types.h"
#include "velaguard/vg_ui.h"
#include "velaguard/vg_ui_lvgl.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifndef __NuttX__
static pthread_t         g_thread;
#endif
static pid_t             g_task;
static volatile bool     g_running;
static lv_obj_t         *g_label;
static lv_obj_t         *g_clock;
static lv_obj_t         *g_bar;
static lv_obj_t         *g_buttons[5];
static lv_obj_t         *g_button_labels[5];
static vg_action_t       g_button_actions[5];
static lv_nuttx_result_t g_result;

extern const lv_font_t lv_font_simsun_16_cjk;

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

static void vg_lvgl_button_event(lv_event_t *event)
{
  uintptr_t index = (uintptr_t)lv_event_get_user_data(event);

  if (index < 5)
    {
      if (vg_ui_page() == VG_PAGE_HOME && index == 0)
        {
          vg_ui_set_page(VG_PAGE_EVENT);
        }
      else if (vg_ui_page() == VG_PAGE_HOME && index == 1)
        {
          vg_ui_set_page(VG_PAGE_HISTORY);
        }
      else if (vg_ui_page() == VG_PAGE_HOME && index == 2)
        {
          vg_ui_set_page(VG_PAGE_TEST);
        }
      else if (vg_ui_page() == VG_PAGE_TEST_MORE && index == 1)
        {
          vg_ui_set_page(VG_PAGE_TEST);
        }
      else if (vg_ui_page() == VG_PAGE_TEST_MORE && index == 3)
        {
          vg_ui_set_page(VG_PAGE_HOME);
        }
      else if (vg_ui_page() == VG_PAGE_HISTORY && index == 3)
        {
          vg_ui_set_page(VG_PAGE_TEST);
        }
      else if (vg_ui_page() == VG_PAGE_TEST_MORE && index == 4)
        {
          vg_ui_set_page(VG_PAGE_HOME);
        }
      else
        {
          vg_input_dispatch(g_button_actions[index]);
        }
    }
}

static void vg_lvgl_set_button(unsigned int index, const char *text,
                               vg_action_t action)
{
  if (index >= 5)
    {
      return;
    }

  g_button_actions[index] = action;
  lv_label_set_text(g_button_labels[index], text);
}

static void vg_lvgl_refresh_buttons(void)
{
  for (unsigned int i = 0; i < 5; i++)
    {
      lv_obj_clear_flag(g_buttons[i], LV_OBJ_FLAG_HIDDEN);
    }

  if (vg_ui_wizard_active())
    {
      vg_lvgl_set_button(0, "上一步", VG_ACT_HANDLED);
      vg_lvgl_set_button(1, vg_ui_wizard_state() == VG_WIZ_RECORDING ?
                         "采集" : "下一项",
                         vg_ui_wizard_state() == VG_WIZ_RECORDING ?
                         VG_ACT_ENTER : VG_ACT_PAGE);
      vg_lvgl_set_button(2, vg_ui_wizard_state() == VG_WIZ_RECORDING ?
                         "保存" : "确认", VG_ACT_ENTER);
      vg_lvgl_set_button(3, "取消", VG_ACT_BACK);
      lv_obj_add_flag(g_buttons[4], LV_OBJ_FLAG_HIDDEN);
      return;
    }

  switch (vg_ui_page())
    {
      case VG_PAGE_EVENT:
        vg_lvgl_set_button(0, "没事了", VG_ACT_HANDLED);
        vg_lvgl_set_button(1, "误报", VG_ACT_FALSE_ALARM);
        vg_lvgl_set_button(2, "稍后提醒", VG_ACT_SNOOZE);
        vg_lvgl_set_button(3, "返回", VG_ACT_BACK);
        break;

      case VG_PAGE_HISTORY:
        vg_lvgl_set_button(0, "上一页", VG_ACT_SCROLL_UP);
        vg_lvgl_set_button(1, "返回", VG_ACT_BACK);
        vg_lvgl_set_button(2, "下一页", VG_ACT_SCROLL_DOWN);
        vg_lvgl_set_button(3, "测试", VG_ACT_PAGE);
        break;

      case VG_PAGE_TEST:
        vg_lvgl_set_button(0, "麦克风", VG_ACT_TEST_MIC);
        vg_lvgl_set_button(1, "播放", VG_ACT_TEST_SPEAKER);
        vg_lvgl_set_button(2, "录入", VG_ACT_ENTER);
        vg_lvgl_set_button(3, "网络", VG_ACT_TEST_NETWORK);
        vg_lvgl_set_button(4, "返回", VG_ACT_BACK);
        break;

      case VG_PAGE_TEST_MORE:
        vg_lvgl_set_button(0, "网络", VG_ACT_TEST_NETWORK);
        vg_lvgl_set_button(1, "测试页", VG_ACT_BACK);
        vg_lvgl_set_button(2, "", VG_ACT_NONE);
        vg_lvgl_set_button(3, "", VG_ACT_NONE);
        vg_lvgl_set_button(4, "首页", VG_ACT_BACK);
        break;

      default:
        vg_lvgl_set_button(0, "事件", VG_ACT_PAGE);
        vg_lvgl_set_button(1, "历史", VG_ACT_NONE);
        vg_lvgl_set_button(2, "测试", VG_ACT_NONE);
        vg_lvgl_set_button(3, "", VG_ACT_NONE);
        vg_lvgl_set_button(4, "", VG_ACT_NONE);
        break;
    }

  if (vg_ui_page() == VG_PAGE_HOME)
    {
      lv_obj_add_flag(g_buttons[3], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_buttons[4], LV_OBJ_FLAG_HIDDEN);
    }
  else if (vg_ui_page() != VG_PAGE_TEST && vg_ui_page() != VG_PAGE_TEST_MORE)
    {
      lv_obj_add_flag(g_buttons[4], LV_OBJ_FLAG_HIDDEN);
    }
}

static void vg_lvgl_refresh(lv_timer_t *timer)
{
  static char text[1024];
  char clock_text[48];
  char time_text[VG_TIMESTR_LEN];

  (void)timer;

  if (vg_ui_render(text, sizeof(text), false) > 0)
    {
      lv_label_set_text(g_label, text);
    }

  vg_format_time(vg_wall_sec(), time_text, sizeof(time_text));
  if (vg_time_reliable())
    {
      snprintf(clock_text, sizeof(clock_text), "现在 %s", time_text + 5);
    }
  else
    {
      snprintf(clock_text, sizeof(clock_text), "时间未同步");
    }
  lv_label_set_text(g_clock, clock_text);

  lv_obj_set_style_bg_color(g_bar, lv_color_hex(vg_level_color()), 0);
  vg_lvgl_refresh_buttons();
}

static void *vg_lvgl_thread(void *arg)
{
  lv_nuttx_dsc_t info;

  (void)arg;

  printf("[velaguard] LVGL线程启动，栈大小=%d\n",
         CONFIG_VELAGUARD_UI_STACKSIZE);
  lv_init();
  printf("[velaguard] LVGL核心初始化完成\n");
  lv_nuttx_dsc_init(&info);
#ifdef CONFIG_LV_USE_NUTTX_LCD
  /* Gemini-S1 exposes the ILI9341 through NuttX's LCD device. */
  info.fb_path = "/dev/lcd0";
#endif

  lv_nuttx_init(&info, &g_result);
  printf("[velaguard] LVGL设备初始化完成，disp=%p\n",
         g_result.disp);

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
  lv_obj_set_height(g_label, 166);
  lv_obj_align(g_label, LV_ALIGN_TOP_LEFT, 6, 16);
  lv_label_set_long_mode(g_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(g_label, &lv_font_simsun_16_cjk, 0);
  lv_obj_set_style_text_color(g_label, lv_color_hex(0xe8eaf0), 0);
  lv_label_set_text(g_label, "安聆守护正在启动...");

  g_clock = lv_label_create(lv_screen_active());
  lv_obj_set_width(g_clock, LV_PCT(96));
  lv_obj_align(g_clock, LV_ALIGN_TOP_RIGHT, -6, 8);
  lv_obj_set_style_text_align(g_clock, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(g_clock, &lv_font_simsun_16_cjk, 0);
  lv_obj_set_style_text_color(g_clock, lv_color_hex(0x9fb6c9), 0);
  lv_label_set_text(g_clock, "时间未同步");

  for (unsigned int i = 0; i < 5; i++)
    {
      g_buttons[i] = lv_button_create(lv_screen_active());
      lv_obj_set_size(g_buttons[i], 60, 40);
      lv_obj_set_pos(g_buttons[i], 2 + (int)i * 64, 194);
      lv_obj_set_style_radius(g_buttons[i], 6, 0);
      lv_obj_set_style_bg_color(g_buttons[i], lv_color_hex(0x28547a), 0);
      lv_obj_set_style_bg_color(g_buttons[i], lv_color_hex(0x3978a8),
                                LV_STATE_PRESSED);
      g_button_labels[i] = lv_label_create(g_buttons[i]);
      lv_obj_set_style_text_font(g_button_labels[i], &lv_font_simsun_16_cjk, 0);
      lv_obj_center(g_button_labels[i]);
      g_button_actions[i] = VG_ACT_NONE;
      lv_obj_add_event_cb(g_buttons[i], vg_lvgl_button_event,
                          LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

  vg_lvgl_refresh_buttons();

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

#ifdef __NuttX__
static int vg_lvgl_task(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  vg_lvgl_thread(NULL);
  return 0;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_ui_lvgl_start(void)
{
  pthread_attr_t attr;
  int ret;

  if (g_running)
    {
      return 0;
    }

  g_running = true;
  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      printf("[velaguard] LVGL线程属性初始化失败: %d\n", ret);
      g_running = false;
      return -1;
    }

  ret = pthread_attr_setstacksize(&attr, CONFIG_VELAGUARD_UI_STACKSIZE);
  if (ret != 0)
    {
      printf("[velaguard] LVGL线程栈设置失败: %d\n", ret);
      pthread_attr_destroy(&attr);
      g_running = false;
      return -1;
    }

#ifdef __NuttX__
  g_task = task_create("velaguard_ui", CONFIG_VELAGUARD_PRIORITY,
                       CONFIG_VELAGUARD_UI_STACKSIZE, vg_lvgl_task, NULL);
  ret = g_task < 0 ? errno : 0;
#else
  ret = pthread_create(&g_thread, &attr, vg_lvgl_thread, NULL);
#endif
  if (ret != 0)
    {
      printf("[velaguard] LVGL线程创建失败: %d (%s)\n",
             ret, strerror(ret));
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
#ifdef __NuttX__
  /* NuttX task has no pthread_join; let its loop observe the stop flag. */
  usleep(200000);
#else
  pthread_join(g_thread, NULL);
#endif
}

bool vg_ui_lvgl_running(void)
{
  return g_running;
}

#endif /* CONFIG_VELAGUARD_UI_LVGL */
