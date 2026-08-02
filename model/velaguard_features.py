#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安聆 VelaGuard - 特征提取（与端侧 C 实现逐步骤对齐）

对应 app/velaguard_device/audio_detector/vg_feature.c。
任何一侧修改都必须同步另一侧，否则电脑端训练指标在板上不成立。

统一约定（PRD-01/07）：
  16kHz 单声道 / 1 秒窗口 / 50% 重叠 / 帧长 25ms / 帧移 10ms
  预加重 0.97、Hamming 窗、512 点 FFT、26 路 Mel(20~7800Hz)、13 维 MFCC
  输出 40 维统计特征：MFCC 均值(13) + 标准差(13) + 一阶差分均值(13) + 过零率(1)
"""

import numpy as np

SAMPLE_RATE = 16000
WINDOW_SAMPLES = 16000
HOP_SAMPLES = WINDOW_SAMPLES // 2
FRAME_LEN = 400
FRAME_HOP = 160
FFT_SIZE = 512
NUM_MEL = 26
NUM_MFCC = 13
FEATURE_DIM = NUM_MFCC * 3 + 1
FEATURE_DIM_V2 = FEATURE_DIM + 13
MAX_FRAMES = 100
PREEMPH = 0.97
MEL_LOW = 20.0
MEL_HIGH = 7800.0
EPS = 1e-10

# v2 谱形特征的频带分界（bin 索引；1kHz=32、4kHz=128、奈奎斯特=256）
BIN_1K = 1000 * FFT_SIZE // SAMPLE_RATE
BIN_4K = 4000 * FFT_SIZE // SAMPLE_RATE
NYQ_BIN = FFT_SIZE // 2


def _hz2mel(hz):
    return 2595.0 * np.log10(1.0 + hz / 700.0)


def _mel2hz(mel):
    return 700.0 * (10.0 ** (mel / 2595.0) - 1.0)


def _mel_filterbank():
    mel_low, mel_high = _hz2mel(MEL_LOW), _hz2mel(MEL_HIGH)
    points = _mel2hz(np.linspace(mel_low, mel_high, NUM_MEL + 2))
    bins = np.floor((FFT_SIZE + 1) * points / SAMPLE_RATE).astype(int)
    bins = np.clip(bins, 0, FFT_SIZE // 2)

    fb = np.zeros((NUM_MEL, FFT_SIZE // 2 + 1), dtype=np.float64)
    for i in range(NUM_MEL):
        lo, mid, hi = bins[i], bins[i + 1], bins[i + 2]
        if mid > lo:
            fb[i, lo:mid] = (np.arange(lo, mid) - lo) / (mid - lo)
        if hi > mid:
            fb[i, mid:hi] = (hi - np.arange(mid, hi)) / (hi - mid)
    return fb


def _dct_matrix():
    n = np.arange(NUM_MEL)
    return np.array(
        [np.cos(np.pi * k * (2 * n + 1) / (2 * NUM_MEL)) for k in range(NUM_MFCC)]
    )


_FB = _mel_filterbank()
_DCT = _dct_matrix()
_HAMMING = 0.54 - 0.46 * np.cos(2 * np.pi * np.arange(FRAME_LEN) / (FRAME_LEN - 1))


def _frames(pcm_int16):
    """分帧主循环：返回 (MFCC 矩阵, 帧功率谱矩阵)。

    与端侧 C 同一循环结构；v2 的帧级能量/谱形统计与 MFCC 共用这一遍
    FFT，端侧不增加第二次分帧开销。
    """
    x = np.asarray(pcm_int16, dtype=np.int16)
    if x.size < FRAME_LEN:
        return (np.zeros((0, NUM_MFCC)),
                np.zeros((0, FFT_SIZE // 2 + 1)))

    mf, pw = [], []
    for n, off in enumerate(range(0, x.size - FRAME_LEN + 1, FRAME_HOP)):
        if n >= MAX_FRAMES:
            break
        frame = x[off:off + FRAME_LEN].astype(np.float64)
        prev = np.empty_like(frame)
        prev[0] = float(x[off - 1]) if off > 0 else 0.0
        prev[1:] = frame[:-1]
        emph = (frame - PREEMPH * prev) * _HAMMING / 32768.0

        spec = np.fft.rfft(emph, n=FFT_SIZE)
        power = (spec.real ** 2 + spec.imag ** 2)
        mel = np.log(_FB @ power + EPS)
        mf.append((_DCT @ mel) / NUM_MEL)
        pw.append(power)

    if not mf:
        return (np.zeros((0, NUM_MFCC)),
                np.zeros((0, FFT_SIZE // 2 + 1)))
    return np.array(mf), np.array(pw)


def mfcc(pcm_int16):
    """返回 (nframes, NUM_MFCC) 的 MFCC 矩阵。"""
    return _frames(pcm_int16)[0]


def extract(pcm_int16):
    """返回 FEATURE_DIM 维特征向量；样本过短返回 None。"""
    x = np.asarray(pcm_int16, dtype=np.int16)
    m = mfcc(x)
    if m.shape[0] == 0:
        return None

    feat = np.zeros(FEATURE_DIM, dtype=np.float64)
    feat[:NUM_MFCC] = m.mean(axis=0)
    feat[NUM_MFCC:NUM_MFCC * 2] = m.std(axis=0)
    if m.shape[0] > 1:
        feat[NUM_MFCC * 2:NUM_MFCC * 3] = np.abs(np.diff(m, axis=0)).mean(axis=0)

    if x.size > 1:
        signs = x >= 0
        feat[-1] = np.count_nonzero(signs[1:] != signs[:-1]) / (x.size - 1)

    return feat


def extract_v2(pcm_int16):
    """v2 = v1 的 40 维原样保留 + 追加 13 维瞬态/时序/谱形描述子。

    动机（2026-08 指标分析）：40 维 MFCC 统计量把窗内时间轴压扁，表达
    不了「短促」（impact/alarm 的瞬态结构）与「音色亮度」（moan 低质心
    vs scream 高质心）；数据量 x8.8、波形增广、类别平衡三个实验均无法
    突破该表征缺口。新维度全部来自 MFCC 同一遍 FFT 的副产品，端侧计算
    增量 <5%。与 vg_feature.c 的同步在训练验证有效后进行。

    追加维度（索引 40-52）：
      40 crest        帧能量峰值 - 均值（dB），瞬态高、稳态低
      41 max_rise     最大帧间能量上升（dB），onset 强度
      42 max_fall     最大帧间能量下降（dB），impact 快衰减 vs alarm 平台
      43 high_ratio   能量高于窗均值的帧占比，瞬态低、稳态约 0.5+
      44-47 seg_env   4 段能量均值 - 全窗均值（dB），粗粒度时序轮廓
      48 cent_mean    谱质心均值（bin/奈奎斯特 bin 归一 0~1）
      49 cent_std     谱质心标准差
      50 cent_dmean   谱质心帧间变化均值（扫频报警 vs 稳定音调）
      51 hf1k_mean    >1kHz 能量占比均值
      52 hf4k_mean    >4kHz 能量占比均值
    """
    x = np.asarray(pcm_int16, dtype=np.int16)
    m, pw = _frames(x)
    if m.shape[0] == 0:
        return None

    feat = np.zeros(FEATURE_DIM_V2, dtype=np.float64)
    feat[:NUM_MFCC] = m.mean(axis=0)
    feat[NUM_MFCC:NUM_MFCC * 2] = m.std(axis=0)
    if m.shape[0] > 1:
        feat[NUM_MFCC * 2:NUM_MFCC * 3] = np.abs(np.diff(m, axis=0)).mean(axis=0)
    if x.size > 1:
        signs = x >= 0
        feat[FEATURE_DIM - 1] = (np.count_nonzero(signs[1:] != signs[:-1])
                                 / (x.size - 1))

    nf = pw.shape[0]
    e_lin = pw.sum(axis=1)                          # 帧能量（线性）
    e_db = 10.0 * np.log10(e_lin + EPS)
    e_mean = e_db.mean()

    feat[40] = e_db.max() - e_mean
    if nf > 1:
        d = np.diff(e_db)
        feat[41] = d.max()
        feat[42] = -d.min()
    feat[43] = float((e_db > e_mean).mean())

    if nf >= 4:
        for i, seg in enumerate(np.array_split(e_db, 4)):
            feat[44 + i] = seg.mean() - e_mean

    bins = np.arange(pw.shape[1], dtype=np.float64)
    cent = (pw @ bins) / (e_lin + EPS) / NYQ_BIN    # 逐帧归一化谱质心
    feat[48] = cent.mean()
    feat[49] = cent.std()
    if nf > 1:
        feat[50] = np.abs(np.diff(cent)).mean()
    feat[51] = float((pw[:, BIN_1K:].sum(axis=1) / (e_lin + EPS)).mean())
    feat[52] = float((pw[:, BIN_4K:].sum(axis=1) / (e_lin + EPS)).mean())

    return feat


def energy_db(pcm_int16):
    x = np.asarray(pcm_int16, dtype=np.float64) / 32768.0
    if x.size == 0:
        return -120.0
    return 20.0 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-9)


def sliding_windows(pcm_int16, window=WINDOW_SAMPLES, hop=HOP_SAMPLES):
    """按 1 秒窗口 / 50% 重叠切片，返回窗口列表。"""
    x = np.asarray(pcm_int16, dtype=np.int16)
    return [x[i:i + window] for i in range(0, max(1, x.size - window + 1), hop)
            if x[i:i + window].size == window]
