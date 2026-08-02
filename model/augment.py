#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安聆 VelaGuard - 训练侧波形增广 (PRD-07)

公开数据里 shout_help(401)/moan(1034)/impact(1857) 的 clip 数远少于
background(14010)，voice 模型训练集里 none 与 shout_help 的窗口比达到 77:1。
本模块在波形域生成同一原始录音的变体，扩充少数类。

三条硬约束：

1. **只用于训练集**。增广样本带 aug 标记，group_split 之后从测试集剔除，
   指标永远是在纯真实样本上算的，否则等于自己给自己放水。
2. **不改变 source 分组**。变体沿用原 clip 的 source，保证同一原始录音的
   原始版与增广版不会一个进训练集一个进测试集。
3. **只做真实录音里会发生的形变**：说话人语速差异、远近距离、房间混响、
   环境底噪。不做频谱掩蔽之类纯数值扰动——那些对 MFCC 统计量特征意义不大。

用法
----
    from augment import augment_pcm
    variant = augment_pcm(pcm_int16, seed=1234)
"""

import numpy as np

SAMPLE_RATE = 16000

# 变速范围：±15%，覆盖不同人语速与情绪急缓；同时改变音高，相当于顺带做了
# 说话人音色扰动（重采样式 speed perturb，Kaldi 的标准做法）
SPEED_RANGE = (0.85, 1.15)

# 混响抽头：家庭房间的早期反射大致在 10-80ms，衰减 0.05-0.3
REVERB_TAPS = (2, 5)
REVERB_DELAY_S = (0.01, 0.08)
REVERB_GAIN = (0.05, 0.30)

# 信噪比：20dB 接近安静客厅，0dB 是电视/抽油烟机开着的极端情况
SNR_RANGE_DB = (0.0, 20.0)

# 距离衰减
GAIN_RANGE = (0.25, 1.0)


def _pink(n, rng):
    """粉噪声：家庭底噪的频谱更接近 1/f 而非白噪。"""
    if n <= 0:
        return np.zeros(0)
    white = rng.standard_normal(n)
    spec = np.fft.rfft(white)
    freqs = np.arange(spec.size)
    freqs[0] = 1
    spec = spec / np.sqrt(freqs)
    out = np.fft.irfft(spec, n=n)
    peak = np.abs(out).max()
    return out / peak if peak > 0 else out


def speed_perturb(x, rate):
    """重采样式变速：时长与音高同时变化。"""
    if abs(rate - 1.0) < 1e-3:
        return x
    n_out = int(round(x.size / rate))
    if n_out < 2:
        return x
    return np.interp(np.linspace(0, x.size - 1, n_out),
                     np.arange(x.size), x)


def add_reverb(x, rng):
    """若干衰减抽头模拟房间早期反射。"""
    out = x.copy()
    for _ in range(rng.integers(*REVERB_TAPS)):
        delay = int(rng.uniform(*REVERB_DELAY_S) * SAMPLE_RATE)
        if delay >= x.size:
            continue
        out[delay:] += rng.uniform(*REVERB_GAIN) * x[:x.size - delay]
    return out


def add_noise(x, rng, snr_db):
    """按目标信噪比混入粉噪声。信号本身接近静音时直接返回，避免除零放大。"""
    sig_pow = np.mean(x ** 2)
    if sig_pow < 1e-9:
        return x
    noise = _pink(x.size, rng)
    noise_pow = np.mean(noise ** 2)
    if noise_pow < 1e-12:
        return x
    scale = np.sqrt(sig_pow / (noise_pow * (10.0 ** (snr_db / 10.0))))
    return x + noise * scale


def augment_pcm(pcm_int16, seed):
    """生成一个变体，返回 int16 PCM。

    seed 决定全部随机量，同一 (clip, seed) 永远得到同一变体，保证可复现。
    """
    rng = np.random.default_rng(seed)
    x = np.asarray(pcm_int16, dtype=np.float64)
    if x.size == 0:
        return np.asarray(pcm_int16, dtype=np.int16)

    x = speed_perturb(x, rng.uniform(*SPEED_RANGE))
    x = add_reverb(x, rng)
    x = add_noise(x, rng, rng.uniform(*SNR_RANGE_DB))
    x = x * rng.uniform(*GAIN_RANGE)

    # 削波会引入原录音没有的谐波，宁可整体缩回来
    peak = np.abs(x).max()
    if peak > 32767.0:
        x = x * (32767.0 / peak)
    return x.astype(np.int16)


def plan(counts, target, skip=()):
    """算出每类要生成几份变体。

    counts: {label: clip 数}；target: 每类期望的最小 clip 数；
    skip: 不增广的类（背景类样本本来就多，增广只会加剧不平衡）。
    返回 {label: 变体份数}，0 表示不增广。
    """
    out = {}
    for label, n in counts.items():
        if label in skip or n <= 0 or n >= target:
            out[label] = 0
        else:
            # n * (1 + k) >= target
            out[label] = int(np.ceil(target / n)) - 1
    return out
