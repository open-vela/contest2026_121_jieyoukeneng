#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安聆 VelaGuard - 自举合成数据集生成器

用途与边界（重要）：

* 本脚本生成的是**参数化合成信号**，不含任何真实家庭录音，可复现、无版权风险，
  用于在真实数据集到位前打通「特征 -> 训练 -> 量化 -> 端侧推理」全流程，并给出
  可编译可运行的自举权重。
* PRD-07 要求的正式指标必须在真实数据集（ESC-50 / FSD50K / 自采）上复现，
  流程见 model/README.md；`prepare_dataset.py` 负责真实音频的重采样与切窗。
* 每个 clip 带 `source` 分组标识，训练脚本按来源分组切分，保证同一"录音来源"
  不会同时出现在训练集与测试集（PRD-07 数据要求）。
"""

import numpy as np

from velaguard_features import SAMPLE_RATE

RNG_SEED = 20260728


def _norm(x, peak=0.7):
    m = np.max(np.abs(x))
    if m < 1e-9:
        return x
    return x / m * peak


def _to_int16(x):
    return np.clip(x * 32767.0, -32768, 32767).astype(np.int16)


def _pink(n, rng):
    """近似粉红噪声（一阶 IIR 低通白噪声叠加）。"""
    white = rng.standard_normal(n)
    b = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
    a = [1, -2.494956002, 2.017265875, -0.522189400]
    y = np.zeros(n)
    for i in range(n):
        acc = b[0] * white[i]
        for k in range(1, 4):
            if i - k >= 0:
                acc += b[k] * white[i - k] - a[k] * y[i - k]
        y[i] = acc
    return y


def _bandpass_noise(n, rng, low, high, order=4):
    """频域矩形带通 + 平滑边沿，避免依赖 scipy。"""
    white = rng.standard_normal(n)
    spec = np.fft.rfft(white)
    freqs = np.fft.rfftfreq(n, 1.0 / SAMPLE_RATE)
    gain = 1.0 / (1.0 + ((freqs - (low + high) / 2) / ((high - low) / 2)) ** (2 * order))
    return np.fft.irfft(spec * gain, n=n)


def _harmonic_voice(n, rng, f0, jitter, nharm, formants, vibrato=0.0):
    """带抖动与共振峰包络的准人声激励。"""
    t = np.arange(n) / SAMPLE_RATE
    f0_track = f0 * (1.0 + jitter * rng.standard_normal(n).cumsum() / max(n, 1) * 10)
    if vibrato > 0:
        f0_track = f0_track * (1.0 + vibrato * np.sin(2 * np.pi * 5.5 * t))
    phase = 2 * np.pi * np.cumsum(f0_track) / SAMPLE_RATE

    sig = np.zeros(n)
    for k in range(1, nharm + 1):
        amp = 1.0 / (k ** 1.1)
        for fc, bw, g in formants:
            amp += g * np.exp(-((k * f0 - fc) ** 2) / (2 * bw ** 2))
        sig += amp * np.sin(k * phase)
    return sig


# --------------------------------------------------------------------------
# 六组声景生成器
# --------------------------------------------------------------------------


def gen_alarm_beep(rng, dur=2.0):
    """烟感/燃气报警：3~3.4kHz 高频蜂鸣，占空比明显的通断脉冲。"""
    n = int(dur * SAMPLE_RATE)
    t = np.arange(n) / SAMPLE_RATE
    f = rng.uniform(2900, 3500)
    period = rng.uniform(0.35, 0.7)
    duty = rng.uniform(0.35, 0.6)
    gate = ((t % period) < period * duty).astype(float)
    # 蜂鸣器接近方波，含明显奇次谐波
    tone = (np.sin(2 * np.pi * f * t)
            + 0.35 * np.sin(2 * np.pi * 3 * f * t)
            + 0.15 * np.sin(2 * np.pi * 5 * f * t))
    env = np.convolve(gate, np.hanning(64) / 32, mode="same")
    sig = tone * np.clip(env, 0, 1)
    return _norm(sig + 0.01 * rng.standard_normal(n))


def gen_water_flow(rng, dur=2.0):
    """持续水流：宽带带通噪声 + 缓慢幅度起伏，无明显谐波结构。"""
    n = int(dur * SAMPLE_RATE)
    sig = _bandpass_noise(n, rng, rng.uniform(600, 1200), rng.uniform(5000, 7200))
    t = np.arange(n) / SAMPLE_RATE
    am = 1.0 + 0.25 * np.sin(2 * np.pi * rng.uniform(0.5, 2.0) * t)
    return _norm(sig * am)


def gen_impact(rng, dur=2.0):
    """破碎/撞击：宽带瞬态冲击 + 指数衰减，可能带碎裂余响。"""
    n = int(dur * SAMPLE_RATE)
    sig = np.zeros(n)
    nhit = rng.integers(1, 3)
    for _ in range(nhit):
        start = rng.integers(0, max(1, n - SAMPLE_RATE // 2))
        length = int(rng.uniform(0.08, 0.30) * SAMPLE_RATE)
        env = np.exp(-np.arange(length) / (length / rng.uniform(3.0, 6.0)))
        burst = rng.standard_normal(length) * env
        # 玻璃碎裂的高频余响
        t = np.arange(length) / SAMPLE_RATE
        burst += 0.4 * env * np.sin(2 * np.pi * rng.uniform(4000, 7000) * t)
        sig[start:start + length] += burst
    return _norm(sig + 0.005 * rng.standard_normal(n))


def gen_moan(rng, dur=2.0):
    """呻吟：低基频、缓慢、低能量、共振峰偏低。"""
    n = int(dur * SAMPLE_RATE)
    sig = _harmonic_voice(n, rng, rng.uniform(95, 165), 0.02, 18,
                          [(500, 120, 0.7), (1100, 200, 0.4)], vibrato=0.01)
    t = np.arange(n) / SAMPLE_RATE
    env = 0.5 + 0.5 * np.sin(2 * np.pi * rng.uniform(0.4, 0.9) * t - np.pi / 2)
    return _norm(sig * env, peak=0.35)


def gen_scream(rng, dur=2.0):
    """痛苦叫声/哭喊：高基频、强谐波、能量高、颤音明显。"""
    n = int(dur * SAMPLE_RATE)
    sig = _harmonic_voice(n, rng, rng.uniform(320, 560), 0.05, 26,
                          [(900, 250, 0.9), (2600, 500, 0.8)], vibrato=0.05)
    t = np.arange(n) / SAMPLE_RATE
    env = np.clip(np.sin(2 * np.pi * rng.uniform(0.6, 1.2) * t) + 0.6, 0, 1.6)
    return _norm(sig * env + 0.05 * rng.standard_normal(n), peak=0.9)


def gen_shout_help(rng, dur=2.0):
    """连续呼救：中高基频，明显的音节通断（约 2~4 音节/秒）。"""
    n = int(dur * SAMPLE_RATE)
    sig = _harmonic_voice(n, rng, rng.uniform(210, 340), 0.04, 22,
                          [(750, 200, 0.8), (1900, 400, 0.6)], vibrato=0.03)
    t = np.arange(n) / SAMPLE_RATE
    rate = rng.uniform(2.0, 4.0)
    syl = (np.sin(2 * np.pi * rate * t) > -0.1).astype(float)
    syl = np.convolve(syl, np.hanning(256) / 128, mode="same")
    return _norm(sig * np.clip(syl, 0, 1), peak=0.8)


def gen_background(rng, dur=2.0):
    """背景噪声与普通说话（负样本）。"""
    n = int(dur * SAMPLE_RATE)
    kind = rng.integers(0, 3)
    if kind == 0:                      # 室内底噪
        sig = _pink(n, rng) * 0.3
    elif kind == 1:                    # 平静交谈
        sig = _harmonic_voice(n, rng, rng.uniform(110, 220), 0.02, 16,
                              [(700, 180, 0.5), (1600, 350, 0.35)])
        t = np.arange(n) / SAMPLE_RATE
        syl = np.convolve((np.sin(2 * np.pi * rng.uniform(3.0, 5.0) * t) > 0).astype(float),
                          np.hanning(400) / 200, mode="same")
        sig = sig * np.clip(syl, 0, 1) * 0.5 + _pink(n, rng) * 0.1
    else:                              # 电视/音乐背景
        t = np.arange(n) / SAMPLE_RATE
        sig = sum(np.sin(2 * np.pi * f * t) / (i + 1)
                  for i, f in enumerate(rng.uniform(180, 2500, 6)))
        sig = sig * (0.6 + 0.4 * np.sin(2 * np.pi * 1.5 * t)) * 0.4
        sig = sig + _pink(n, rng) * 0.25
    return _norm(sig, peak=rng.uniform(0.15, 0.45))


GENERATORS = {
    "alarm_beep": gen_alarm_beep,
    "water_flow": gen_water_flow,
    "impact": gen_impact,
    "moan": gen_moan,
    "scream": gen_scream,
    "shout_help": gen_shout_help,
    "background": gen_background,
}


def add_room_effect(sig, rng):
    """叠加距离衰减、房间混响与本底噪声，模拟近/远距离与家庭背景。"""
    n = sig.size
    # 简易混响：若干衰减抽头
    out = sig.copy()
    for _ in range(rng.integers(2, 5)):
        delay = int(rng.uniform(0.01, 0.08) * SAMPLE_RATE)
        gain = rng.uniform(0.05, 0.25)
        out[delay:] += gain * sig[:n - delay]

    gain = rng.uniform(0.25, 1.0)                 # 近/远距离
    noise = _pink(n, rng) * rng.uniform(0.005, 0.06)
    return _norm(out * gain + noise, peak=min(0.95, rng.uniform(0.3, 0.9)))


def build_dataset(clips_per_class=60, dur=2.0, seed=RNG_SEED):
    """返回 (features_ready_clips, labels, sources)。

    每个 clip 是一段 int16 PCM；source 形如 "alarm_beep#003"，
    同一 source 的所有滑窗只会落在同一个数据集划分中。
    """
    rng = np.random.default_rng(seed)
    clips, labels, sources = [], [], []

    for name, fn in GENERATORS.items():
        for i in range(clips_per_class):
            base = fn(rng, dur=dur)
            sig = add_room_effect(base, rng)
            clips.append(_to_int16(sig))
            labels.append(name)
            sources.append(f"{name}#{i:03d}")

    return clips, labels, sources


if __name__ == "__main__":
    import wave
    import os
    import sys

    outdir = sys.argv[1] if len(sys.argv) > 1 else "synth_samples"
    os.makedirs(outdir, exist_ok=True)
    clips, labels, sources = build_dataset(clips_per_class=2)
    for pcm, label, src in zip(clips, labels, sources):
        path = os.path.join(outdir, f"{src.replace('#', '_')}.wav")
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(pcm.tobytes())
    print(f"已写出 {len(clips)} 个合成样本到 {outdir}/")
