/****************************************************************************
 * 安聆 VelaGuard - 端侧分类器 (PRD-01/07)
 ****************************************************************************/

#ifndef __VELAGUARD_VG_CLASSIFIER_H
#define __VELAGUARD_VG_CLASSIFIER_H

#include "velaguard/vg_model.h"
#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 环境声景分类。probs 至少 VG_ENV_CLASSES 个 float，可为 NULL。
 * 返回 argmax 对应的 vg_sound_class_t。
 */

vg_sound_class_t vg_classify_env(const float *feat, float *probs);

/* 人声异常检测。返回 vg_voice_kind_t（不含 name_call/help_phrase，
 * 后两者由 enrollment 模板匹配产生）。
 */

vg_voice_kind_t vg_classify_voice(const float *feat, float *probs);

/* 模型信息，用于 status 命令与交付材料 */

const char *vg_classifier_info(void);

/* 单次推理耗时统计（微秒），用于 PRD-01 性能验收 */

void vg_classifier_perf(uint32_t *count, uint32_t *avg_us, uint32_t *max_us);
void vg_classifier_perf_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_CLASSIFIER_H */
