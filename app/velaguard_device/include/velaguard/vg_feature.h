/****************************************************************************
 * 安聆 VelaGuard - 声学特征提取 (PRD-01/07)
 *
 * 统一 16kHz 单声道、1 秒窗口、50% 重叠；帧长 25ms、帧移 10ms。
 * v2 特征（2026-08）：40 维 MFCC 统计（均值/标准差/一阶差分均值+过零率）
 * + 13 维瞬态/时序/谱形描述子（能量峰比、起落沿、分段包络、谱质心、
 * 高频占比）。新维度全部来自 MFCC 同一遍 FFT 的副产品，实测把 impact
 * 事件级 F1 从 0.46 抬到 0.76、moan/scream 由谱质心直接可分。
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
#define VG_NUM_EXTRA       13               /* v2 追加的描述子 */
#define VG_FEATURE_DIM     (VG_NUM_MFCC * 3 + 1 + VG_NUM_EXTRA)  /* = 53 */
#define VG_MAX_FRAMES      100

/* v2 谱形特征的频带分界（FFT bin；1kHz=32、4kHz=128、奈奎斯特=256） */

#define VG_BIN_1K          (1000 * VG_FFT_SIZE / VG_SAMPLE_RATE)
#define VG_BIN_4K          (4000 * VG_FFT_SIZE / VG_SAMPLE_RATE)
#define VG_NYQ_BIN         (VG_FFT_SIZE / 2)

/* v2 追加维度的索引（与 model/velaguard_features.py extract_v2 一致）
 *   40 crest      帧能量峰值-均值(dB)   41 max_rise  最大帧间上升(dB)
 *   42 max_fall   最大帧间下降(dB)      43 high_ratio 高能帧占比
 *   44-47 seg_env 4 段能量均值-全窗均值  48 cent_mean 谱质心均值(归一)
 *   49 cent_std   谱质心标准差          50 cent_dmean 质心帧间变化
 *   51 hf1k_mean  >1kHz 能量占比        52 hf4k_mean >4kHz 能量占比
 */

#define VG_FEAT_ZCR        (VG_NUM_MFCC * 3)        /* 39 */
#define VG_FEAT_CREST      40
#define VG_FEAT_CENT_MEAN  48
#define VG_FEAT_HF4K       52

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
