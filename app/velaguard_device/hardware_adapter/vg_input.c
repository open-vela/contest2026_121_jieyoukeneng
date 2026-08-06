/****************************************************************************
 * 安聆 VelaGuard - 本地输入实现 (PRD-04)
 ****************************************************************************/

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "velaguard/vg_event_sm.h"
#include "velaguard/vg_input.h"
#include "velaguard/vg_ui.h"

#ifndef CONFIG_VELAGUARD_BUTTON_DEVICE
#  define CONFIG_VELAGUARD_BUTTON_DEVICE "/dev/buttons"
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int      g_btn_fd = -1;
static uint32_t g_last_mask;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_input_init(void)
{
  g_btn_fd = open(CONFIG_VELAGUARD_BUTTON_DEVICE, O_RDONLY | O_NONBLOCK);
  if (g_btn_fd < 0)
    {
      printf("[velaguard] 未检测到按键设备(%s)，"
             "请用串口命令 velaguard key <动作> 操作\n",
             CONFIG_VELAGUARD_BUTTON_DEVICE);
    }

  g_last_mask = 0;
  return 0;
}

void vg_input_deinit(void)
{
  if (g_btn_fd >= 0)
    {
      close(g_btn_fd);
      g_btn_fd = -1;
    }
}

bool vg_input_has_buttons(void)
{
  return g_btn_fd >= 0;
}

vg_action_t vg_input_poll(void)
{
  uint8_t raw = 0;
  uint32_t mask;
  uint32_t pressed;

  if (g_btn_fd < 0)
    {
      return VG_ACT_NONE;
    }

  if (read(g_btn_fd, &raw, sizeof(raw)) != (ssize_t)sizeof(raw))
    {
      return VG_ACT_NONE;
    }

  mask = raw;
  pressed = mask & ~g_last_mask;   /* 只取上升沿，天然去抖 */
  g_last_mask = mask;

  if (pressed & 0x01)
    {
      return VG_ACT_PAGE;
    }

  if (pressed & 0x02)
    {
      return VG_ACT_HANDLED;
    }

  if (pressed & 0x04)
    {
      return VG_ACT_FALSE_ALARM;
    }

  if (pressed & 0x08)
    {
      return VG_ACT_SNOOZE;
    }

  if (pressed & 0x10)
    {
      return VG_ACT_ENTER;
    }

  return VG_ACT_NONE;
}

vg_action_t vg_input_parse(const char *s)
{
  if (s == NULL)
    {
      return VG_ACT_NONE;
    }

  if (strcmp(s, "page") == 0)        return VG_ACT_PAGE;
  if (strcmp(s, "handled") == 0)     return VG_ACT_HANDLED;
  if (strcmp(s, "false") == 0 ||
      strcmp(s, "false_alarm") == 0) return VG_ACT_FALSE_ALARM;
  if (strcmp(s, "snooze") == 0)      return VG_ACT_SNOOZE;
  if (strcmp(s, "enter") == 0)       return VG_ACT_ENTER;
  if (strcmp(s, "back") == 0)        return VG_ACT_BACK;
  if (strcmp(s, "up") == 0)          return VG_ACT_SCROLL_UP;
  if (strcmp(s, "down") == 0)        return VG_ACT_SCROLL_DOWN;

  return VG_ACT_NONE;
}

void vg_input_dispatch(vg_action_t action)
{
  switch (action)
    {
      case VG_ACT_PAGE:
        if (vg_ui_wizard_active())
          {
            vg_ui_wizard_next();
          }
        else
          {
            vg_ui_next_page();
          }
        break;

      case VG_ACT_HANDLED:
        if (vg_ui_wizard_active())
          {
            vg_ui_wizard_prev();
          }
        else
          {
            vg_sm_ack(NULL, VG_STATUS_HANDLED);
          }
        break;

      case VG_ACT_FALSE_ALARM:
        if (!vg_ui_wizard_active())
          {
            vg_sm_ack(NULL, VG_STATUS_FALSE_ALARM);
          }
        break;

      case VG_ACT_SNOOZE:
        if (!vg_ui_wizard_active())
          {
            if (vg_sm_ack(NULL, VG_STATUS_SNOOZED) == -2)
              {
                printf("[velaguard] 稍后提醒次数已用尽，"
                       "本次超时将按无人响应升级\n");
              }
          }
        break;

      case VG_ACT_ENTER:
        if (vg_ui_wizard_active())
          {
            vg_ui_wizard_confirm();
          }
        else if (vg_ui_page() == VG_PAGE_HOME ||
                 vg_ui_page() == VG_PAGE_TEST)
          {
            vg_ui_wizard_start();
          }
        break;

      case VG_ACT_BACK:
        if (vg_ui_wizard_active())
          {
            vg_ui_wizard_cancel();
          }
        else
          {
            vg_ui_set_page(vg_ui_page() == VG_PAGE_TEST_MORE ?
                           VG_PAGE_TEST : VG_PAGE_HOME);
          }
        break;

      case VG_ACT_SCROLL_UP:
        vg_ui_history_scroll(-1);
        break;

      case VG_ACT_SCROLL_DOWN:
        vg_ui_history_scroll(1);
        break;

      case VG_ACT_TEST_MIC:
        vg_diagnostics_start(VG_DIAG_MIC);
        break;

      case VG_ACT_TEST_SPEAKER:
        vg_diagnostics_start(VG_DIAG_SPEAKER);
        break;

      case VG_ACT_TEST_NETWORK:
        vg_diagnostics_start(VG_DIAG_NETWORK);
        break;

      default:
        break;
    }
}
