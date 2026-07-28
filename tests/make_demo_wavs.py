#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安聆 VelaGuard - 生成演示 / 回归用 wav

产物写入 tests/.vgtest/wav/（已在 .gitignore 中排除，不进仓库）。
这些是**参数化合成信号**，不含任何真实家庭录音。

  python3 make_demo_wavs.py            # 生成全部
  python3 make_demo_wavs.py alarm_beep # 只生成一个
"""

import os
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "model"))

import synth_dataset as sd            # noqa: E402
from velaguard_features import SAMPLE_RATE   # noqa: E402

OUT = os.path.join(HERE, ".vgtest", "wav")

# 时长按状态机规则设计：水流要足够长才能走到警告级（演示阈值 25 秒）
PLAN = {
    "alarm_beep": 8.0,
    "water_flow": 40.0,
    "impact": 4.0,
    "moan": 8.0,
    "scream": 6.0,
    "shout_help": 8.0,
    "background": 20.0,
}


def write(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(np.asarray(pcm, dtype=np.int16).tobytes())


def main():
    os.makedirs(OUT, exist_ok=True)
    rng = np.random.default_rng(2026)
    only = sys.argv[1] if len(sys.argv) > 1 else None

    for name, dur in PLAN.items():
        if only and name != only:
            continue
        sig = sd.add_room_effect(sd.GENERATORS[name](rng, dur=dur), rng)
        path = os.path.join(OUT, f"{name}.wav")
        write(path, np.clip(sig * 32767.0, -32768, 32767).astype(np.int16))
        print(f"{path}  {dur:.0f}s")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
