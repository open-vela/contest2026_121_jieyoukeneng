/****************************************************************************
 * 安聆 VelaGuard - 声学特征提取 (PRD-01/07)
 *
 * 统一 16kHz 单声道、1 秒窗口、50% 重叠；帧长 25ms、帧移 10ms；
 * 输出 MFCC 统计特征向量（均值 / 标准差 / 一阶差分均值 + 过零率）。
 *
 * 该实现不依赖任何第三方推理框架，符合 PRD-07 的降级路线要求。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_FEATURE_H
#define __VELAGUARD_VG_FEATURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define VG_SAMPLE_RATE     16000
#define VG_WINDOW_SAMPLES  16000            /* 1 秒 */
#define VG_HOP_SAMPLES     (VG_WINDOW_SAMPLES / 2) /* 50% 重叠 */
#define VG_FRAME_LEN       400              /* 25ms */
#define VG_FRAME_HOP       160              /* 10ms */
#define VG_FFT_SIZE        512
#define VG_NUM_MEL         26
#define VG_NUM_MFCC        13
#define VG_FEATURE_DIM     (VG_NUM_MFCC * 3 + 1)  /* = 40 */
#define VG_MAX_FRAMES      100

/* 从一段 16kHz 单声道 PCM 提取特征向量。
 * pcm 长度不足 VG_FRAME_LEN 时返回 -1。
 * feat 至少 VG_FEATURE_DIM 个 float。
 */

int vg_feature_extract(const int16_t *pcm, size_t nsamples, float *feat);

/* 供单元测试与训练脚本对齐使用：直接输出每帧 MFCC。
 * mfcc 需要 VG_MAX_FRAMES * VG_NUM_MFCC 个 float，返回帧数。
 */

int vg_feature_mfcc(const int16_t *pcm, size_t nsamples, float *mfcc);

/* 窗口整体能量（dBFS），用于静音判定与急促程度估计 */

float vg_feature_energy_db(const int16_t *pcm, size_t nsamples);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_FEATURE_H */
