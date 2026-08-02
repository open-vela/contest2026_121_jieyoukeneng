/****************************************************************************
 * 安聆 VelaGuard - 端侧模型定义 (PRD-07)
 *
 * 采用 "MFCC 统计特征 + 两层 MLP" 的轻量方案：无第三方推理运行时、
 * 权重以 C 数组直接编入固件，满足闪存 <= 2MB / 单窗口 < 500ms 的预算，
 * 同时是 PRD-07 降级路线的落地形态。
 *
 * 权重文件 vg_model_weights.c 由 model/train_models.py 自动生成，
 * 请勿手工编辑。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_MODEL_H
#define __VELAGUARD_VG_MODEL_H

#include "velaguard/vg_feature.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 环境声景分类：other / alarm_beep / water_flow / impact */

#define VG_ENV_CLASSES   4
#define VG_ENV_HIDDEN    48

/* 人声异常检测：none / distress
 *
 * 2026-08 定稿：moan/scream/shout_help 合并为 distress。三类在状态机里的
 * 处置本就同路（VG_EVT_DISTRESS_VOICE：本地询问 -> 重复升级），而三类
 * 两两互混恰是错误主体（四类结构事件级宏 F1 仅 0.54，合并后 0.89）。
 * 急促程度改由检测器用能量/谱质心估计，产品行为不变。
 */

#define VG_VOICE_CLASSES 2
#define VG_VOICE_HIDDEN  32

/* 由训练脚本生成的权重（int8 量化 + per-tensor scale） */

extern const char  g_vg_model_version[];
extern const char  g_vg_model_trained_on[];

extern const float g_vg_env_mean[VG_FEATURE_DIM];
extern const float g_vg_env_scale[VG_FEATURE_DIM];
extern const signed char g_vg_env_w1[VG_ENV_HIDDEN][VG_FEATURE_DIM];
extern const float g_vg_env_w1_scale;
extern const float g_vg_env_b1[VG_ENV_HIDDEN];
extern const signed char g_vg_env_w2[VG_ENV_CLASSES][VG_ENV_HIDDEN];
extern const float g_vg_env_w2_scale;
extern const float g_vg_env_b2[VG_ENV_CLASSES];

extern const float g_vg_voice_mean[VG_FEATURE_DIM];
extern const float g_vg_voice_scale[VG_FEATURE_DIM];
extern const signed char g_vg_voice_w1[VG_VOICE_HIDDEN][VG_FEATURE_DIM];
extern const float g_vg_voice_w1_scale;
extern const float g_vg_voice_b1[VG_VOICE_HIDDEN];
extern const signed char g_vg_voice_w2[VG_VOICE_CLASSES][VG_VOICE_HIDDEN];
extern const float g_vg_voice_w2_scale;
extern const float g_vg_voice_b2[VG_VOICE_CLASSES];

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_MODEL_H */
