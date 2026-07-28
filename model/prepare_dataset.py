#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安聆 VelaGuard - 真实音频预处理 (PRD-07)

把任意采样率 / 声道的 wav 统一为 16kHz 单声道，并按 2 秒片段切分，
输出到训练脚本约定的目录结构。

用法：
  python3 prepare_dataset.py --src raw_audio --dst ../datasets

输入目录结构（自行整理，原始音频不进 Git）：
  raw_audio/alarm_beep/*.wav
  raw_audio/water_flow/*.wav
  raw_audio/impact/*.wav
  raw_audio/moan/*.wav
  raw_audio/scream/*.wav
  raw_audio/shout_help/*.wav
  raw_audio/background/*.wav

输出文件名形如 `<原文件名>__000.wav`，训练脚本据此按「录音来源」分组切分，
保证同一原始录音不会同时进入训练集与测试集。
"""

import argparse
import os
import sys
import wave

import numpy as np

CLASSES = ["alarm_beep", "water_flow", "impact",
           "moan", "scream", "shout_help", "background"]
TARGET_SR = 16000
SEGMENT_SEC = 2.0


def read_wav(path):
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        ch = w.getnchannels()
        sw = w.getsampwidth()
        raw = w.readframes(w.getnframes())

    if sw == 2:
        data = np.frombuffer(raw, dtype=np.int16).astype(np.float64)
    elif sw == 1:
        data = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128) * 256
    elif sw == 4:
        data = np.frombuffer(raw, dtype=np.int32).astype(np.float64) / 65536.0
    else:
        raise ValueError(f"{path}: 不支持的位宽 {sw * 8}bit")

    if ch > 1:
        data = data.reshape(-1, ch).mean(axis=1)
    return data, sr


def resample_linear(x, sr_in, sr_out=TARGET_SR):
    if sr_in == sr_out:
        return x
    n_out = int(round(x.size * sr_out / sr_in))
    if n_out <= 1:
        return np.zeros(0)
    t_in = np.arange(x.size)
    t_out = np.linspace(0, x.size - 1, n_out)
    return np.interp(t_out, t_in, x)


def write_wav(path, pcm_float):
    pcm = np.clip(pcm_float, -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(TARGET_SR)
        w.writeframes(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="原始音频目录")
    ap.add_argument("--dst", default="../datasets", help="输出目录")
    ap.add_argument("--segment", type=float, default=SEGMENT_SEC)
    ap.add_argument("--min-rms", type=float, default=1e-4,
                    help="低于该 RMS 的静音片段直接丢弃")
    args = ap.parse_args()

    seg = int(args.segment * TARGET_SR)
    total = 0

    for cls in CLASSES:
        src_dir = os.path.join(args.src, cls)
        if not os.path.isdir(src_dir):
            continue

        dst_dir = os.path.join(args.dst, cls)
        os.makedirs(dst_dir, exist_ok=True)
        n_cls = 0

        for name in sorted(os.listdir(src_dir)):
            if not name.lower().endswith(".wav"):
                continue

            try:
                data, sr = read_wav(os.path.join(src_dir, name))
            except Exception as exc:                     # noqa: BLE001
                print(f"  跳过 {name}: {exc}", file=sys.stderr)
                continue

            data = resample_linear(data, sr)
            stem = os.path.splitext(name)[0].replace("__", "_")

            for i, off in enumerate(range(0, max(1, data.size - seg + 1), seg)):
                chunk = data[off:off + seg]
                if chunk.size < seg:
                    break
                if np.sqrt(np.mean((chunk / 32768.0) ** 2)) < args.min_rms:
                    continue
                write_wav(os.path.join(dst_dir, f"{stem}__{i:03d}.wav"), chunk)
                n_cls += 1

        print(f"{cls:12s}: {n_cls} 个片段")
        total += n_cls

    print(f"合计 {total} 个 2 秒片段 -> {args.dst}")
    print("下一步：python3 train_models.py --data-dir", args.dst)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
