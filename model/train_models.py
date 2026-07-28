#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安聆 VelaGuard - 训练 / 评估 / int8 量化导出 (PRD-07)

产出：
  1. app/velaguard_device/audio_detector/vg_model_weights.c  端侧权重（int8）
  2. model/reports/metrics.md                                指标报告
  3. model/reports/metrics.json                              机器可读指标

用法：
  python3 train_models.py                        # 用自举合成数据集训练
  python3 train_models.py --data-dir ../datasets  # 用真实数据集训练（推荐）

真实数据集目录结构（由 prepare_dataset.py 生成）：
  datasets/alarm_beep/*.wav  datasets/water_flow/*.wav  datasets/impact/*.wav
  datasets/moan/*.wav        datasets/scream/*.wav      datasets/shout_help/*.wav
  datasets/background/*.wav

数据切分严格按「录音来源」分组（同一原始录音不跨集），符合 PRD-07 数据要求。
"""

import argparse
import json
import os
import sys
import wave
from datetime import datetime, timezone

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import velaguard_features as vf   # noqa: E402
import synth_dataset as sd        # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WEIGHTS_OUT = os.path.join(
    HERE, "..", "app", "velaguard_device", "audio_detector", "vg_model_weights.c")
REPORT_DIR = os.path.join(HERE, "reports")

ENV_CLASSES = ["other", "alarm_beep", "water_flow", "impact"]
VOICE_CLASSES = ["none", "moan", "scream", "shout_help"]
ENV_HIDDEN = 24
VOICE_HIDDEN = 16

ENV_MAP = {"alarm_beep": 1, "water_flow": 2, "impact": 3}
VOICE_MAP = {"moan": 1, "scream": 2, "shout_help": 3}


# --------------------------------------------------------------------------
# 数据装载
# --------------------------------------------------------------------------

def load_wav(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2 or w.getframerate() != vf.SAMPLE_RATE:
            raise ValueError(
                f"{path}: 需要 16bit/{vf.SAMPLE_RATE}Hz，请先运行 prepare_dataset.py")
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
        if w.getnchannels() == 2:
            data = data.reshape(-1, 2).mean(axis=1).astype(np.int16)
    return data


def load_real_dataset(data_dir):
    clips, labels, sources = [], [], []
    for label in sd.GENERATORS:
        d = os.path.join(data_dir, label)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.lower().endswith(".wav"):
                continue
            clips.append(load_wav(os.path.join(d, name)))
            labels.append(label)
            # 来源分组：同一原始录音切出的所有片段共享前缀（prepare_dataset 约定
            # 文件名形如 <source>__<seq>.wav）
            sources.append(f"{label}#{name.split('__')[0]}")
    return clips, labels, sources


def featurize(clips, labels, sources):
    X, Y, G = [], [], []
    for pcm, label, src in zip(clips, labels, sources):
        for win in vf.sliding_windows(pcm):
            f = vf.extract(win)
            if f is not None:
                X.append(f)
                Y.append(label)
                G.append(src)
    return np.array(X), np.array(Y), np.array(G)


def group_split(groups, test_ratio=0.3, seed=7):
    uniq = np.unique(groups)
    rng = np.random.default_rng(seed)
    rng.shuffle(uniq)
    n_test = max(1, int(len(uniq) * test_ratio))
    test_groups = set(uniq[:n_test])
    mask_test = np.array([g in test_groups for g in groups])
    return ~mask_test, mask_test


# --------------------------------------------------------------------------
# 两层 MLP（numpy 手写，与端侧 C 前向严格一致）
# --------------------------------------------------------------------------

class MLP:
    def __init__(self, dim, hidden, classes, seed=0):
        rng = np.random.default_rng(seed)
        self.w1 = rng.standard_normal((hidden, dim)) * np.sqrt(2.0 / dim)
        self.b1 = np.zeros(hidden)
        self.w2 = rng.standard_normal((classes, hidden)) * np.sqrt(2.0 / hidden)
        self.b2 = np.zeros(classes)

    def forward(self, X):
        z1 = X @ self.w1.T + self.b1
        h = np.maximum(z1, 0.0)
        z2 = h @ self.w2.T + self.b2
        z2 = z2 - z2.max(axis=1, keepdims=True)
        e = np.exp(z2)
        return h, z1, e / e.sum(axis=1, keepdims=True)

    def predict(self, X):
        return self.forward(X)[2]

    def train(self, X, y, classes, epochs=600, lr=3e-3, l2=1e-4,
              class_weight=None, seed=0, verbose=False):
        n, _ = X.shape
        onehot = np.eye(classes)[y]
        if class_weight is None:
            class_weight = np.ones(classes)
        w = class_weight[y][:, None]

        params = [self.w1, self.b1, self.w2, self.b2]
        m = [np.zeros_like(p) for p in params]
        v = [np.zeros_like(p) for p in params]
        b1c, b2c, eps = 0.9, 0.999, 1e-8
        rng = np.random.default_rng(seed)

        for ep in range(1, epochs + 1):
            idx = rng.permutation(n)
            Xb, yb, wb, ob = X[idx], y[idx], w[idx], onehot[idx]

            h, z1, p = self.forward(Xb)
            loss = -np.sum(wb * ob * np.log(p + 1e-12)) / n

            dz2 = (p - ob) * wb / n
            gw2 = dz2.T @ h + l2 * self.w2
            gb2 = dz2.sum(axis=0)
            dh = dz2 @ self.w2
            dz1 = dh * (z1 > 0)
            gw1 = dz1.T @ Xb + l2 * self.w1
            gb1 = dz1.sum(axis=0)

            grads = [gw1, gb1, gw2, gb2]
            for i, (p_, g) in enumerate(zip(params, grads)):
                m[i] = b1c * m[i] + (1 - b1c) * g
                v[i] = b2c * v[i] + (1 - b2c) * g * g
                mh = m[i] / (1 - b1c ** ep)
                vh = v[i] / (1 - b2c ** ep)
                p_ -= lr * mh / (np.sqrt(vh) + eps)

            if verbose and ep % 100 == 0:
                print(f"    epoch {ep:4d}  loss={loss:.4f}")
        return self


# --------------------------------------------------------------------------
# 评估
# --------------------------------------------------------------------------

def evaluate(model, X, y, classes, names):
    pred = model.predict(X).argmax(axis=1)
    cm = np.zeros((classes, classes), dtype=int)
    for t, p in zip(y, pred):
        cm[t, p] += 1

    per = {}
    f1s = []
    for c in range(classes):
        tp = cm[c, c]
        fn = cm[c].sum() - tp
        fp = cm[:, c].sum() - tp
        prec = tp / (tp + fp) if tp + fp else 0.0
        rec = tp / (tp + fn) if tp + fn else 0.0
        f1 = 2 * prec * rec / (prec + rec) if prec + rec else 0.0
        per[names[c]] = {"precision": prec, "recall": rec, "f1": f1,
                         "support": int(cm[c].sum())}
        f1s.append(f1)

    return {
        "accuracy": float((pred == y).mean()),
        "macro_f1": float(np.mean(f1s)),
        "per_class": per,
        "confusion": cm.tolist(),
    }


def fmt_confusion(cm, names):
    w = max(len(n) for n in names) + 2
    head = "真\\预".ljust(w) + "".join(n.ljust(w) for n in names)
    lines = ["```", head]
    for i, n in enumerate(names):
        lines.append(n.ljust(w) + "".join(str(v).ljust(w) for v in cm[i]))
    lines.append("```")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# int8 量化导出
# --------------------------------------------------------------------------

def quantize(w):
    scale = float(np.max(np.abs(w))) / 127.0
    if scale < 1e-12:
        scale = 1.0
    q = np.clip(np.round(w / scale), -127, 127).astype(np.int8)
    return q, scale


def c_float_array(name, arr, per_line=8):
    lines = [f"const float {name}[{len(arr)}] ="]
    lines.append("{")
    for i in range(0, len(arr), per_line):
        chunk = ", ".join(f"{v:.6f}f" for v in arr[i:i + per_line])
        lines.append(f"  {chunk},")
    lines.append("};")
    return "\n".join(lines)


def c_int8_matrix(name, mat, per_line=16):
    rows, cols = mat.shape
    lines = [f"const signed char {name}[{rows}][{cols}] =", "{"]
    for r in range(rows):
        lines.append("  {")
        for i in range(0, cols, per_line):
            chunk = ", ".join(f"{int(v):4d}" for v in mat[r, i:i + per_line])
            lines.append(f"    {chunk},")
        lines.append("  },")
    lines.append("};")
    return "\n".join(lines)


def export_weights(path, env, env_norm, voice, voice_norm, trained_on, version):
    qe1, se1 = quantize(env.w1)
    qe2, se2 = quantize(env.w2)
    qv1, sv1 = quantize(voice.w1)
    qv2, sv2 = quantize(voice.w2)

    parts = [
        "/****************************************************************************",
        " * 安聆 VelaGuard - 端侧模型权重（自动生成，请勿手工编辑）",
        " *",
        f" * 生成时间 : {datetime.now(timezone.utc).astimezone().isoformat(timespec='seconds')}",
        f" * 生成脚本 : model/train_models.py",
        f" * 训练数据 : {trained_on}",
        " *",
        " * 结构     : 40 维 MFCC 统计特征 -> 两层 MLP（int8 权重 + float 偏置）",
        " ****************************************************************************/",
        "",
        '#include "velaguard/vg_model.h"',
        "",
        f'const char g_vg_model_version[] = "{version}";',
        f'const char g_vg_model_trained_on[] = "{trained_on}";',
        "",
        "/* ---- 环境声景分类：other / alarm_beep / water_flow / impact ---- */",
        "",
        c_float_array("g_vg_env_mean", env_norm[0]),
        "",
        c_float_array("g_vg_env_scale", env_norm[1]),
        "",
        c_int8_matrix("g_vg_env_w1", qe1),
        "",
        f"const float g_vg_env_w1_scale = {se1:.9f}f;",
        "",
        c_float_array("g_vg_env_b1", env.b1),
        "",
        c_int8_matrix("g_vg_env_w2", qe2),
        "",
        f"const float g_vg_env_w2_scale = {se2:.9f}f;",
        "",
        c_float_array("g_vg_env_b2", env.b2),
        "",
        "/* ---- 人声异常检测：none / moan / scream / shout_help ---- */",
        "",
        c_float_array("g_vg_voice_mean", voice_norm[0]),
        "",
        c_float_array("g_vg_voice_scale", voice_norm[1]),
        "",
        c_int8_matrix("g_vg_voice_w1", qv1),
        "",
        f"const float g_vg_voice_w1_scale = {sv1:.9f}f;",
        "",
        c_float_array("g_vg_voice_b1", voice.b1),
        "",
        c_int8_matrix("g_vg_voice_w2", qv2),
        "",
        f"const float g_vg_voice_w2_scale = {sv2:.9f}f;",
        "",
        c_float_array("g_vg_voice_b2", voice.b2),
        "",
    ]

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))

    total = qe1.nbytes + qe2.nbytes + qv1.nbytes + qv2.nbytes
    total += (env.b1.size + env.b2.size + voice.b1.size + voice.b2.size) * 4
    total += (env_norm[0].size + env_norm[1].size +
              voice_norm[0].size + voice_norm[1].size) * 4
    return total


class QuantModel:
    """用量化后的权重做前向，验证量化损失（与端侧 C 一致）。"""

    def __init__(self, model):
        self.q1, self.s1 = quantize(model.w1)
        self.b1 = model.b1
        self.q2, self.s2 = quantize(model.w2)
        self.b2 = model.b2

    def predict(self, X):
        h = np.maximum(X @ self.q1.astype(np.float64).T * self.s1 + self.b1, 0.0)
        z = h @ self.q2.astype(np.float64).T * self.s2 + self.b2
        z = z - z.max(axis=1, keepdims=True)
        e = np.exp(z)
        return e / e.sum(axis=1, keepdims=True)


# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default=None,
                    help="真实数据集目录（缺省使用自举合成数据集）")
    ap.add_argument("--clips", type=int, default=60,
                    help="合成模式下每类 clip 数")
    ap.add_argument("--epochs", type=int, default=800)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--no-export", action="store_true")
    args = ap.parse_args()

    if args.data_dir:
        clips, labels, sources = load_real_dataset(args.data_dir)
        if not clips:
            print(f"错误：{args.data_dir} 下没有找到任何 wav", file=sys.stderr)
            return 1
        trained_on = f"real:{os.path.basename(os.path.abspath(args.data_dir))}"
    else:
        clips, labels, sources = sd.build_dataset(clips_per_class=args.clips)
        trained_on = "synthetic-bootstrap"

    print(f"[1/5] 装载 {len(clips)} 个 clip（{trained_on}）")
    X, Y, G = featurize(clips, labels, sources)
    print(f"[2/5] 切窗提特征：{X.shape[0]} 个 1 秒窗口 x {X.shape[1]} 维")

    tr, te = group_split(G, seed=args.seed)
    print(f"      按录音来源分组切分：训练 {tr.sum()} / 测试 {te.sum()}")

    results = {}
    exported = {}

    for tag, mapping, names, hidden in (
            ("env", ENV_MAP, ENV_CLASSES, ENV_HIDDEN),
            ("voice", VOICE_MAP, VOICE_CLASSES, VOICE_HIDDEN)):
        y = np.array([mapping.get(l, 0) for l in Y])
        mean = X[tr].mean(axis=0)
        std = X[tr].std(axis=0)
        std[std < 1e-6] = 1.0
        Xn = (X - mean) / std

        counts = np.bincount(y[tr], minlength=len(names)).astype(float)
        counts[counts == 0] = 1.0
        cw = counts.sum() / (len(names) * counts)

        print(f"[3/5] 训练 {tag} 模型（hidden={hidden}）")
        model = MLP(vf.FEATURE_DIM, hidden, len(names), seed=args.seed)
        model.train(Xn[tr], y[tr], len(names), epochs=args.epochs,
                    class_weight=cw, seed=args.seed, verbose=True)

        res_f = evaluate(model, Xn[te], y[te], len(names), names)
        qres = evaluate(QuantModel(model), Xn[te], y[te], len(names), names)
        results[tag] = {"float": res_f, "int8": qres}
        exported[tag] = (model, (mean, std))
        print(f"      float  macro-F1={res_f['macro_f1']:.3f} "
              f"acc={res_f['accuracy']:.3f}")
        print(f"      int8   macro-F1={qres['macro_f1']:.3f} "
              f"acc={qres['accuracy']:.3f}")

    version = datetime.now(timezone.utc).strftime("%Y%m%d")
    size = 0
    if not args.no_export:
        os.makedirs(os.path.dirname(WEIGHTS_OUT), exist_ok=True)
        size = export_weights(WEIGHTS_OUT,
                              exported["env"][0], exported["env"][1],
                              exported["voice"][0], exported["voice"][1],
                              trained_on, version)
        print(f"[4/5] 已导出端侧权重 -> {os.path.relpath(WEIGHTS_OUT, HERE)} "
              f"（{size} 字节，预算 <= 1MB）")

    os.makedirs(REPORT_DIR, exist_ok=True)
    payload = {
        "trained_on": trained_on,
        "version": version,
        "windows": int(X.shape[0]),
        "feature_dim": int(X.shape[1]),
        "weight_bytes": size,
        "results": results,
    }
    with open(os.path.join(REPORT_DIR, "metrics.json"), "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)

    lines = [
        "# 安聆 VelaGuard 模型指标报告",
        "",
        f"- 训练数据：`{trained_on}`",
        f"- 模型版本：`{version}`",
        f"- 样本窗口：{X.shape[0]} 个（1 秒 / 50% 重叠）",
        f"- 特征维度：{X.shape[1]}（MFCC 均值+标准差+差分均值+过零率）",
        f"- 权重体积：{size} 字节（PRD-07 预算：两模型合计 ≤ 1MB）",
        f"- 切分方式：按录音来源分组，同一来源不跨训练/测试集",
        "",
        "> PRD-07 验收线：宏平均 F1 ≥ 0.80，各安全类别召回 ≥ 0.80。",
        "",
    ]
    if trained_on == "synthetic-bootstrap":
        lines += [
            "> ⚠️ **本报告基于自举合成信号数据集**，类别间人为可分，指标偏乐观，"
            "**不能**作为 PRD-07 验收依据。其唯一作用是证明"
            "「特征 → 训练 → int8 量化 → 端侧 C 推理」链路已打通、"
            "并提供一份可编译可运行的初始权重。",
            "> 正式指标须按 model/README.md 的流程，在 ESC-50 / FSD50K / 自采真实"
            "录音上重新训练并复现。",
            "",
        ]
    for tag, names in (("env", ENV_CLASSES), ("voice", VOICE_CLASSES)):
        for prec in ("float", "int8"):
            r = results[tag][prec]
            lines += [
                f"## {tag} 模型 / {prec}",
                "",
                f"- 准确率：{r['accuracy']:.3f}",
                f"- 宏平均 F1：{r['macro_f1']:.3f}",
                "",
                "| 类别 | 精确率 | 召回率 | F1 | 样本数 |",
                "|---|---|---|---|---|",
            ]
            for n in names:
                p = r["per_class"][n]
                lines.append(f"| {n} | {p['precision']:.3f} | {p['recall']:.3f} "
                             f"| {p['f1']:.3f} | {p['support']} |")
            lines += ["", "混淆矩阵：", fmt_confusion(np.array(r["confusion"]), names), ""]

    with open(os.path.join(REPORT_DIR, "metrics.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"[5/5] 指标报告 -> {os.path.relpath(REPORT_DIR, HERE)}/metrics.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
