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
import multiprocessing
import os
import sys
import wave
import zlib
from datetime import datetime, timezone

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import velaguard_features as vf   # noqa: E402
import synth_dataset as sd        # noqa: E402
import augment as ag              # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WEIGHTS_OUT = os.path.join(
    HERE, "..", "app", "velaguard_device", "audio_detector", "vg_model_weights.c")
REPORT_DIR = os.path.join(HERE, "reports")

ENV_CLASSES = ["other", "alarm_beep", "water_flow", "impact"]
VOICE_CLASSES = ["none", "moan", "scream", "shout_help"]
ENV_HIDDEN = 48
VOICE_HIDDEN = 32

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


def scan_real_dataset(data_dir):
    """只扫路径不读音频，返回 [(path, label, source)]。

    26468 条 clip 的 PCM 一次性读进内存约 4.6GB，8GB 机器会打满并落到 swap；
    改为传路径给工作进程、各自读各自释放，峰值内存与数据集规模无关。
    """
    files = []
    for label in sd.GENERATORS:
        d = os.path.join(data_dir, label)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.lower().endswith(".wav"):
                continue
            # 来源分组：同一原始录音切出的所有片段共享前缀（prepare_dataset 约定
            # 文件名形如 <source>__<seq>.wav）
            files.append((os.path.join(d, name), label,
                          f"{label}#{name.split('__')[0]}"))
    return files


# 弱标签能量过滤：公开集的长 clip（AudioSet 10 秒、ASVP 数秒）事件往往只占
# 其中一小段，其余窗口实为安静背景，全按 clip 标签训练会大量污染正样本。
# 正样本类只保留能量在「该 clip 峰值窗口 12dB 以内」且不低于绝对底线的窗口；
# background 不过滤（安静本来就是它的一部分）。
ENERGY_KEEP_WITHIN_DB = 12.0
ENERGY_ABS_FLOOR_DB = -55.0


def clip_features(pcm, label):
    """一个 clip -> 若干窗口特征。"""
    wins = vf.sliding_windows(pcm)
    if label != "background" and len(wins) > 1:
        dbs = [vf.energy_db(w) for w in wins]
        thr = max(max(dbs) - ENERGY_KEEP_WITHIN_DB, ENERGY_ABS_FLOOR_DB)
        wins = [w for w, db in zip(wins, dbs) if db >= thr]
    return [f for f in (_EXTRACT(w) for w in wins) if f is not None]


# 特征提取入口：--features v2 时切到 vf.extract_v2（Linux fork 语义下
# 工作进程继承该全局，无需逐任务传递）
_EXTRACT = vf.extract


def _feat_worker(task):
    """工作进程入口：读盘 ->（增广）-> 切窗 -> 提特征。

    mfcc() 是逐帧 Python 循环，22 万窗口单进程要跑 40 分钟；这里按 clip 切分
    并行。velaguard_features 不动——它是与端侧 C 逐维对齐的参考实现。
    """
    path, label, src, aug_id = task
    pcm = load_wav(path)
    if aug_id:
        # seed 由来源与序号决定：同一命令重跑得到同一批变体。用 crc32 而非
        # 内置 hash()——后者对 str 带进程级随机盐，跨进程跨运行都不一致
        seed = (zlib.crc32(src.encode()) ^ (aug_id * 2654435761)) & 0xFFFFFFFF
        pcm = ag.augment_pcm(pcm, seed=seed)
    return clip_features(pcm, label), label, src, bool(aug_id)


def featurize_files(tasks, workers):
    """并行提特征。返回 X, Y, G, A（A 标记该窗口是否来自增广变体）。"""
    X, Y, G, A = [], [], [], []
    done = 0
    with multiprocessing.Pool(workers) as pool:
        for feats, label, src, is_aug in pool.imap_unordered(
                _feat_worker, tasks, chunksize=16):
            for f in feats:
                X.append(f)
                Y.append(label)
                G.append(src)
                A.append(is_aug)
            done += 1
            if done % 4000 == 0:
                print(f"      已处理 {done}/{len(tasks)} 个 clip，"
                      f"{len(X)} 个窗口", flush=True)
    return X, Y, G, A


def featurize_items(items, synthetic=False):
    """串行版，用于内存里的合成 clip（数量少，不值得起进程池）。

    synthetic=True 时窗口带非真实标记：与增广样本同样只进训练集，
    测试集与指标始终是纯真实域（--mix-synth 的权重要认识合成回归夹具，
    但指标不能被人为可分的合成信号抬高）。
    """
    X, Y, G, A = [], [], [], []
    n = 0
    for pcm, label, src in items:
        n += 1
        for f in clip_features(pcm, label):
            X.append(f)
            Y.append(label)
            G.append(src)
            A.append(synthetic)
    return X, Y, G, A, n



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
            # 阶梯衰减：前 50% 全速，50%-80% 降到 0.3x，最后 20% 降到 0.1x
            if ep <= epochs * 0.5:
                cur_lr = lr
            elif ep <= epochs * 0.8:
                cur_lr = lr * 0.3
            else:
                cur_lr = lr * 0.1

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
                p_ -= cur_lr * mh / (np.sqrt(vh) + eps)

            if verbose and ep % 100 == 0:
                print(f"    epoch {ep:4d}  loss={loss:.4f}")
        return self


# --------------------------------------------------------------------------
# 评估
# --------------------------------------------------------------------------

def _score(y, pred, classes, names):
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
        "accuracy": float((np.asarray(pred) == np.asarray(y)).mean()),
        "macro_f1": float(np.mean(f1s)),
        "per_class": per,
        "confusion": cm.tolist(),
    }


def evaluate(model, X, y, classes, names):
    pred = model.predict(X).argmax(axis=1)
    return _score(y, pred, classes, names)


def evaluate_events(model, X, y, groups, classes, names):
    """事件级评估：按原始录音来源聚合窗口概率（软投票）后判类。

    产品链路里单窗口误判会被检测器的多窗口聚合滤掉，实机验收
    （PRD-08：每类 >=20 次触发、成功率 >=80%）本来就是事件级口径；
    这里给出与之对应的离线指标。窗口级指标仍如实并列输出。
    """
    prob = model.predict(X)
    uniq, inv = np.unique(groups, return_inverse=True)
    agg = np.zeros((len(uniq), prob.shape[1]))
    np.add.at(agg, inv, prob)
    pred = agg.argmax(axis=1)

    y_ev = np.zeros(len(uniq), dtype=int)
    y_ev[inv] = y                    # 同一来源的窗口标签一致，重复赋值无害
    return _score(y_ev, pred, classes, names)


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


def export_weights(path, env, env_norm, voice, voice_norm, trained_on, version,
                   env_names, voice_names):
    qe1, se1 = quantize(env.w1)
    qe2, se2 = quantize(env.w2)
    qv1, sv1 = quantize(voice.w1)
    qv2, sv2 = quantize(voice.w2)
    dim = env_norm[0].size

    parts = [
        "/****************************************************************************",
        " * 安聆 VelaGuard - 端侧模型权重（自动生成，请勿手工编辑）",
        " *",
        f" * 生成时间 : {datetime.now(timezone.utc).astimezone().isoformat(timespec='seconds')}",
        f" * 生成脚本 : model/train_models.py",
        f" * 训练数据 : {trained_on}",
        " *",
        f" * 结构     : {dim} 维声学统计特征 -> 两层 MLP（int8 权重 + float 偏置）",
        " ****************************************************************************/",
        "",
        '#include "velaguard/vg_model.h"',
        "",
        f'const char g_vg_model_version[] = "{version}";',
        f'const char g_vg_model_trained_on[] = "{trained_on}";',
        "",
        f"/* ---- 环境声景分类：{' / '.join(env_names)} ---- */",
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
        f"/* ---- 人声异常检测：{' / '.join(voice_names)} ---- */",
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
    ap.add_argument("--mix-synth", type=int, default=0,
                    help="真实数据训练时每类混入 N 条合成 clip：让模型同时覆盖 "
                         "tests/ 的合成回归夹具与 velaguard sim 演示信号（仓库"
                         "不进音频，夹具只能运行时合成，模型必须两个域都认）")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--no-export", action="store_true")
    ap.add_argument("--augment-to", type=int, default=0,
                    help="把每个正样本类的 clip 数用波形增广补到至少 N 条；"
                         "增广样本只进训练集，测试集永远是纯真实样本")
    ap.add_argument("--balance", type=float, default=0.0,
                    help="训练集里负类(other/none)窗口数最多为正类总和的 N 倍，"
                         "超出部分随机丢弃；0 表示不做平衡")
    ap.add_argument("--workers", type=int, default=max(1, os.cpu_count() - 1),
                    help="特征提取并行进程数")
    ap.add_argument("--merge-voice", action="store_true",
                    help="额外训练一个 moan/scream/shout_help 合并为 distress "
                         "的二分类 voice 模型（训练侧实验，不动端侧；三类在"
                         "状态机里的处置本就同路）")
    ap.add_argument("--final", action="store_true",
                    help="定稿模式（2026-08 拍板方案）：只训练 env 四类 + "
                         "voice 二分类（distress 合并），导出的 voice 权重"
                         "即二分类模型")
    ap.add_argument("--features", choices=("v1", "v2"), default="v1",
                    help="特征版本：v1=40 维现网特征；v2=追加瞬态/谱形描述子"
                         "（Python 侧实验，验证有效后才同步端侧 C）")
    args = ap.parse_args()

    global _EXTRACT
    if args.features == "v2":
        _EXTRACT = vf.extract_v2

    aug_note = ""
    if args.data_dir:
        files = scan_real_dataset(args.data_dir)
        if not files:
            print(f"错误：{args.data_dir} 下没有找到任何 wav", file=sys.stderr)
            return 1
        trained_on = f"real:{os.path.basename(os.path.abspath(args.data_dir))}"

        tasks = [(p, l, s, 0) for p, l, s in files]
        if args.augment_to > 0:
            per_class = {}
            for _, label, _ in files:
                per_class[label] = per_class.get(label, 0) + 1
            # 背景类样本本来就压倒性多，再增广只会加剧不平衡
            aug_plan = ag.plan(per_class, args.augment_to, skip=("background",))
            for p, l, s in files:
                for k in range(1, aug_plan.get(l, 0) + 1):
                    tasks.append((p, l, s, k))
            used = {k: v for k, v in aug_plan.items() if v}
            if used:
                aug_note = "，".join(f"{k} x{v + 1}" for k, v in sorted(used.items()))
                print(f"      增广计划（每类补到 {args.augment_to} 条）：{aug_note}")
                trained_on += f"+aug{args.augment_to}"

        print(f"[1/5] 装载并切窗（{trained_on}）：{len(files)} 个 clip"
              f"，含增广共 {len(tasks)} 个任务，{args.workers} 进程")
        X, Y, G, A = featurize_files(tasks, args.workers)
        n_clips = len(files)

        if args.mix_synth > 0:
            sclips, slabels, ssources = sd.build_dataset(
                clips_per_class=args.mix_synth)
            sX, sY, sG, sA, sn = featurize_items(
                zip(sclips, slabels, ssources), synthetic=True)
            X += sX
            Y += sY
            G += sG
            A += sA
            n_clips += sn
            trained_on += f"+synth{args.mix_synth}"
    else:
        clips, labels, sources = sd.build_dataset(clips_per_class=args.clips)
        trained_on = "synthetic-bootstrap"
        print(f"[1/5] 装载并切窗（{trained_on}）")
        X, Y, G, A, n_clips = featurize_items(zip(clips, labels, sources))

    X = np.array(X)
    Y = np.array(Y)
    G = np.array(G)
    A = np.array(A, dtype=bool)
    if args.features == "v2":
        trained_on += "+featv2"
    print(f"[2/5] {n_clips} 个 clip -> {X.shape[0]} 个 1 秒窗口 x {X.shape[1]} 维"
          f"（其中增广/合成 {int(A.sum())} 个，只进训练集）")

    tr, te = group_split(G, seed=args.seed)
    # 增广与合成样本一律不进测试集：指标必须在纯真实样本上算
    te = te & ~A
    print(f"      按录音来源分组切分：训练 {tr.sum()} / 测试 {te.sum()}（纯真实）")

    specs = [("env", ENV_MAP, ENV_CLASSES, ENV_HIDDEN)]
    if args.final:
        # 定稿结构：voice 槽位即二分类（none/distress）
        specs.append(("voice", {"moan": 1, "scream": 1, "shout_help": 1},
                      ["none", "distress"], VOICE_HIDDEN))
    else:
        specs.append(("voice", VOICE_MAP, VOICE_CLASSES, VOICE_HIDDEN))
        if args.merge_voice:
            specs.append(("voice_merged",
                          {"moan": 1, "scream": 1, "shout_help": 1},
                          ["none", "distress"], VOICE_HIDDEN))
    spec_names = {tag: names for tag, _, names, _ in specs}

    results = {}
    exported = {}

    for tag, mapping, names, hidden in specs:
        y = np.array([mapping.get(l, 0) for l in Y])

        # 训练集平衡：负类(other/none)窗口远多于正类（voice 侧曾达 77:1），
        # 随机丢弃超额负类样本。注意两个模型的"负类"含义不同，各算各的。
        tr_idx = np.where(tr)[0]
        if args.balance > 0:
            pos = tr_idx[y[tr_idx] != 0]
            neg = tr_idx[y[tr_idx] == 0]
            cap = int(len(pos) * args.balance)
            if len(neg) > cap:
                rng = np.random.default_rng(args.seed)
                neg = rng.choice(neg, cap, replace=False)
                tr_idx = np.sort(np.concatenate([pos, neg]))
                print(f"      {tag} 训练集平衡：负类 {int((y[np.where(tr)[0]] == 0).sum())}"
                      f" -> {len(neg)}，正类 {len(pos)}")

        mean = X[tr_idx].mean(axis=0)
        std = X[tr_idx].std(axis=0)
        std[std < 1e-6] = 1.0
        Xn = (X - mean) / std

        counts = np.bincount(y[tr_idx], minlength=len(names)).astype(float)
        counts[counts == 0] = 1.0
        # sqrt 回火的逆频率权重：完全逆频率会把极少数类（impact 仅数百 clip）
        # 放大几十倍，实测导致其精确率崩溃（0.07-0.10，大量误报）；
        # sqrt 缓和后归一到均值 1，兼顾少数类召回与多数类精确率
        cw = np.sqrt(counts.sum() / counts)
        cw = cw / cw.mean()

        print(f"[3/5] 训练 {tag} 模型（hidden={hidden}）")
        model = MLP(X.shape[1], hidden, len(names), seed=args.seed)
        model.train(Xn[tr_idx], y[tr_idx], len(names), epochs=args.epochs,
                    class_weight=cw, seed=args.seed, verbose=True)

        res_f = evaluate(model, Xn[te], y[te], len(names), names)
        qres = evaluate(QuantModel(model), Xn[te], y[te], len(names), names)
        ev_f = evaluate_events(model, Xn[te], y[te], G[te], len(names), names)
        ev_q = evaluate_events(QuantModel(model), Xn[te], y[te], G[te],
                               len(names), names)
        results[tag] = {"float": res_f, "int8": qres,
                        "float_event": ev_f, "int8_event": ev_q}
        exported[tag] = (model, (mean, std))
        print(f"      窗口级 float macro-F1={res_f['macro_f1']:.3f} "
              f"acc={res_f['accuracy']:.3f}")
        print(f"      窗口级 int8  macro-F1={qres['macro_f1']:.3f} "
              f"acc={qres['accuracy']:.3f}")
        print(f"      事件级 int8  macro-F1={ev_q['macro_f1']:.3f} "
              f"acc={ev_q['accuracy']:.3f}（按原始录音软投票）")

    version = datetime.now(timezone.utc).strftime("%Y%m%d")
    size = 0
    if not args.no_export:
        os.makedirs(os.path.dirname(WEIGHTS_OUT), exist_ok=True)
        size = export_weights(WEIGHTS_OUT,
                              exported["env"][0], exported["env"][1],
                              exported["voice"][0], exported["voice"][1],
                              trained_on, version,
                              spec_names["env"], spec_names["voice"])
        print(f"[4/5] 已导出端侧权重 -> {os.path.relpath(WEIGHTS_OUT, HERE)} "
              f"（{size} 字节，预算 <= 1MB）")

    os.makedirs(REPORT_DIR, exist_ok=True)
    payload = {
        "trained_on": trained_on,
        "version": version,
        "windows": int(X.shape[0]),
        "windows_augmented": int(A.sum()),
        "feature_dim": int(X.shape[1]),
        "features": args.features,
        "weight_bytes": size,
        "balance_ratio": args.balance,
        "results": results,
    }
    with open(os.path.join(REPORT_DIR, "metrics.json"), "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)

    lines = [
        "# 安聆 VelaGuard 模型指标报告",
        "",
        f"- 训练数据：`{trained_on}`",
        f"- 模型版本：`{version}`",
        f"- 样本窗口：{X.shape[0]} 个（1 秒 / 50% 重叠）"
        + (f"，其中增广/合成 {int(A.sum())} 个（只进训练集，指标为纯真实域）"
           if A.any() else ""),
        f"- 特征维度：{X.shape[1]}（MFCC 均值+标准差+差分均值+过零率）",
        f"- 权重体积：{size} 字节（PRD-07 预算：两模型合计 ≤ 1MB）",
        f"- 切分方式：按录音来源分组，同一来源不跨训练/测试集",
    ]
    if args.balance > 0:
        lines.append(f"- 训练集平衡：负类窗口上限为正类的 {args.balance} 倍")
    lines += [
        "",
        "> PRD-07 验收线：宏平均 F1 ≥ 0.80，各安全类别召回 ≥ 0.80。",
        "> 测试集为纯真实样本（增广变体一律排除）。",
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
    for tag in results:
        names = spec_names[tag]
        for key, title in (("float", "float / 窗口级"),
                           ("int8", "int8 / 窗口级"),
                           ("int8_event", "int8 / 事件级（按原始录音软投票）")):
            r = results[tag][key]
            lines += [
                f"## {tag} 模型 / {title}",
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
