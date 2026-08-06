/****************************************************************************
 * 安聆 VelaGuard - 本地提醒指示 (PRD-04)
 *
 * 按原理图修正：板上无独立蜂鸣器，声音走扬声器 media 播放接口；
 * 灯光走板载 WS2812 RGB LED（守护绿 / 警告黄 / 紧急红）。
 *
 * 硬件不可用时（模拟器线）自动降级为控制台指示，本地闭环仍完整可演示。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_INDICATOR_H
#define __VELAGUARD_VG_INDICATOR_H

#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
  VG_LED_OFF = 0,
  VG_LED_GUARD,        /* 守护中：绿色呼吸 */
  VG_LED_PAUSED,       /* 守护暂停：蓝色常亮 */
  VG_LED_NOTICE,       /* 提醒：绿色快闪一次 */
  VG_LED_WARNING,      /* 警告：黄色闪烁 */
  VG_LED_EMERGENCY     /* 紧急：红色急闪 */
} vg_led_mode_t;

int  vg_indicator_init(void);
void vg_indicator_deinit(void);

/* 设置 LED 表现（内部按 tick 产生呼吸/闪烁节奏） */

void vg_indicator_set_led(vg_led_mode_t mode);
vg_led_mode_t vg_indicator_led(void);

/* 播放分级提示音：
 *   notice    短音一次
 *   warning   间隔音 + 倒计时
 *   emergency 急促连续音
 * 播放期间按 PRD-04 时分方案让路给采集（单次 <= 2 秒）。
 */

int  vg_indicator_play(vg_level_t level);

/* LCD 背光唤醒 */

void vg_indicator_backlight_wake(void);

/* 周期驱动（建议 100ms） */

void vg_indicator_tick(void);

/* 当前 LED 颜色描述，用于控制台与 UI 显示 */

const char *vg_indicator_led_text(void);

/* 是否检测到真实 LED / 扬声器硬件 */

bool vg_indicator_has_led(void);
bool vg_indicator_has_audio_out(void);

/* 录放并发能力标定结果（实机基线第一周验证，PRD-01/04） */

void vg_indicator_set_full_duplex(bool supported);
bool vg_indicator_full_duplex(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_INDICATOR_H */
