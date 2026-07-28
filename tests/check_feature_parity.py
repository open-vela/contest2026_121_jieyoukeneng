#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安聆 VelaGuard - 端侧 C 特征实现与训练脚本特征实现的一致性校验

两侧特征定义必须逐维一致，否则电脑端训练出的指标在板上不成立。
本脚本用同一段 wav 分别跑：
  * C:      tests/build/vg_featdump
  * Python: model/velaguard_features.py
逐维比较，最大绝对误差超过阈值即判定失败。
"""

import os
import subprocess
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "model"))

import velaguard_features as vf      # noqa: E402
import synth_dataset as sd           # noqa: E402

TOL = 2e-3


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(vf.SAMPLE_RATE)
        w.writeframes(np.asarray(pcm, dtype=np.int16).tobytes())


def main():
    outdir = os.path.join(HERE, ".vgtest")
    os.makedirs(outdir, exist_ok=True)
    dump = os.path.join(HERE, "build", "vg_featdump")

    if not os.path.exists(dump):
        print("缺少 build/vg_featdump，请先 make", file=sys.stderr)
        return 1

    rng = np.random.default_rng(4242)
    worst = 0.0
    checked = 0

    for name, gen in sd.GENERATORS.items():
        wav = os.path.join(outdir, f"parity_{name}.wav")
        pcm = np.clip(sd.add_room_effect(gen(rng, dur=3.0), rng) * 32767.0,
                      -32768, 32767).astype(np.int16)
        write_wav(wav, pcm)

        out = subprocess.run([dump, wav], capture_output=True, text=True,
                             check=True).stdout.strip().splitlines()
        # 过滤掉 vg_capture 打印的音频源提示，只保留特征行
        out = [l for l in out if l.startswith("window ")]

        windows = vf.sliding_windows(pcm, window=vf.WINDOW_SAMPLES,
                                     hop=vf.WINDOW_SAMPLES)
        if len(out) != len(windows):
            print(f"✗ {name}: 窗口数不一致 C={len(out)} PY={len(windows)}")
            return 1

        for line, win in zip(out, windows):
            c_feat = np.array([float(x) for x in line.split()[2:]])
            py_feat = vf.extract(win)
            diff = float(np.max(np.abs(c_feat - py_feat)))
            worst = max(worst, diff)
            checked += 1
            if diff > TOL:
                bad = int(np.argmax(np.abs(c_feat - py_feat)))
                print(f"✗ {name}: 第 {bad} 维差异 {diff:.6f} 超过阈值 {TOL}")
                print(f"   C ={c_feat[bad]:.6f}  PY={py_feat[bad]:.6f}")
                return 1

    print(f"✓ 特征一致性通过：{checked} 个窗口 × {vf.FEATURE_DIM} 维，"
          f"最大绝对误差 {worst:.2e}（阈值 {TOL}）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
