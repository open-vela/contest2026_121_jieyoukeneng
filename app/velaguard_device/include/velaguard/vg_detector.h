/****************************************************************************
 * 安聆 VelaGuard - 声学识别引擎调度 (PRD-01)
 *
 * 三条识别通道并行工作于同一个 1 秒窗口（50% 重叠）：
 *   1. 环境声景分类   -> SoundObservation
 *   2. 人声异常检测   -> DistressObservation
 *   3. 个性化模板匹配 -> DistressObservation(name_call / help_phrase)
 *
 * 本模块只输出类别与置信度，不做任何告警决策（决策在 PRD-03 状态机）。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_DETECTOR_H
#define __VELAGUARD_VG_DETECTOR_H

#include "velaguard/vg_capture.h"
#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
  uint32_t         windows;         /* 已处理窗口数 */
  uint32_t         sound_hits;      /* 环境声景命中数 */
  uint32_t         voice_hits;      /* 人声异常命中数 */
  uint32_t         template_hits;   /* 个性化模板命中数 */
  float            last_energy_db;
  vg_sound_class_t last_sound;
  float            last_sound_conf;
  vg_voice_kind_t  last_voice;
  float            last_voice_conf;
  uint32_t         avg_us;          /* 单窗口平均处理耗时 */
  uint32_t         max_us;          /* 单窗口最大处理耗时 */
} vg_detector_stats_t;

int  vg_detector_init(void);
void vg_detector_deinit(void);

/* 处理一个完整的 1 秒窗口（16000 样本），产生观测并投喂状态机 */

int vg_detector_process_window(const int16_t *pcm, size_t nsamples);

/* 从当前音频源拉取一个 hop（0.5 秒）并在窗口就绪时处理。
 * 返回处理的窗口数：0 表示暂无数据（暂停 / 无音频源 / wav 结束）。
 */

int vg_detector_pump(void);

/* 离线跑完整个 wav：模拟器线用真实识别链路验证五类声景 */

int vg_detector_run_file(const char *path);

void vg_detector_stats(vg_detector_stats_t *out);
void vg_detector_stats_reset(void);

/* 静音门限以下的窗口直接跳过（省电且避免噪声误报） */

void vg_detector_set_silence_db(float db);

/* 空跑模式：只做特征与推理、不把观测投喂给状态机。
 * 用于性能标定（velaguard bench），避免合成信号污染真实事件日志。
 */

void vg_detector_set_dry_run(bool on);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_DETECTOR_H */
