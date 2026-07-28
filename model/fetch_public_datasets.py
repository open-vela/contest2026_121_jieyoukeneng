#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安聆 VelaGuard - 公开数据集获取与映射 (PRD-07)

把 ESC-50 / FSD50K 的公开音频映射成 train_models.py 约定的目录结构，
统一转成 16kHz 单声道，并按「原始录音来源」命名，保证训练/测试分组切分成立。

调研结论与类别映射依据见 model/dataset_sources.md。

用法
----
  # 1) ESC-50（约 600MB，一条命令搞定）
  python3 fetch_public_datasets.py --esc50 --dst ../datasets

  # 2) FSD50K 元数据（仅 7MB）：看清单与许可证分布，再决定要不要下 25GB 音频
  python3 fetch_public_datasets.py --fsd50k-meta

  # 3) 已经把 FSD50K 音频解压到本地后，映射进来
  python3 fetch_public_datasets.py --fsd50k-map /path/to/FSD50K --dst ../datasets

  # 只保留授权最干净的 clip（默认只要 CC0 与 CC-BY）
  python3 fetch_public_datasets.py --fsd50k-map /path/to/FSD50K \\
      --licenses cc0,by --dst ../datasets

命名约定
--------
输出文件名为 `<source>__<seq>.wav`，`<source>` 是原始录音标识：
  ESC-50  -> esc50-<src_file>      （官方 meta 里的 Freesound 原始文件 ID）
  FSD50K  -> fsd50k-<clip_id>      （每个 clip 即一个独立来源）
train_models.py 按 `__` 前缀分组切分，同一原始录音不会跨训练/测试集。
"""

import argparse
import csv
import io
import json
import os
import sys
import time
import urllib.request
import wave
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from prepare_dataset import read_wav, resample_linear, write_wav, TARGET_SR  # noqa: E402

ESC50_ZIP = "https://github.com/karolpiczak/ESC-50/archive/master.zip"
ESC50_META = "https://raw.githubusercontent.com/karolpiczak/ESC-50/master/meta/esc50.csv"
ZENODO_FSD50K = "https://zenodo.org/api/records/4060432"

# --- 类别映射（依据 model/dataset_sources.md，类别名均已核实存在）-------------

ESC50_MAP = {
    "clock_alarm": "alarm_beep",
    "siren": "alarm_beep",
    "pouring_water": "water_flow",
    "water_drops": "water_flow",
    "toilet_flush": "water_flow",
    "glass_breaking": "impact",
    "door_wood_knock": "impact",
    "crying_baby": "scream",
    # 负样本：家居与非语言人声
    "vacuum_cleaner": "background",
    "washing_machine": "background",
    "clock_tick": "background",
    "keyboard_typing": "background",
    "mouse_click": "background",
    "can_opening": "background",
    "door_wood_creaks": "background",
    "footsteps": "background",
    "laughing": "background",
    "breathing": "background",
    "coughing": "background",
    "sneezing": "background",
    "snoring": "background",
    "brushing_teeth": "background",
    "drinking_sipping": "background",
    "clapping": "background",
}

FSD50K_MAP = {
    "Alarm": "alarm_beep",
    "Siren": "alarm_beep",
    "Doorbell": "alarm_beep",
    "Chime": "alarm_beep",
    "Water": "water_flow",
    "Water_tap_and_faucet": "water_flow",
    "Sink_(filling_or_washing)": "water_flow",
    "Bathtub_(filling_or_washing)": "water_flow",
    "Toilet_flush": "water_flow",
    "Boiling": "water_flow",
    "Glass": "impact",
    "Shatter": "impact",
    "Thump_and_thud": "impact",
    "Knock": "impact",
    "Screaming": "scream",
    "Crying_and_sobbing": "scream",
    "Shout": "shout_help",
    "Yell": "shout_help",
    "Speech": "background",
    "Conversation": "background",
    "Dishes_and_pots_and_pans": "background",
    "Frying_(food)": "background",
}

# 一条 clip 若同时带这些"冲突标签"，说明内容混杂，直接跳过，避免正负样本互相污染
CONFLICT = {
    "alarm_beep": {"Speech", "Conversation", "Music"},
    "water_flow": {"Speech", "Conversation", "Music"},
    "impact": {"Speech", "Conversation", "Music"},
    "scream": {"Music"},
    "shout_help": {"Music"},
    "background": {"Alarm", "Siren", "Screaming", "Shatter", "Glass"},
}


def fetch(url, tries=3, timeout=60):
    """带重试的下载（本机走系统代理即可）。"""
    err = None
    for _ in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=timeout) as r:
                return r.read()
        except Exception as exc:                        # noqa: BLE001
            err = exc
            time.sleep(3)
    raise RuntimeError(f"下载失败 {url}: {err}")


def convert(src_bytes_or_path, dst_path):
    """任意 wav -> 16kHz 单声道 16bit。"""
    if isinstance(src_bytes_or_path, bytes):
        tmp = dst_path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(src_bytes_or_path)
        try:
            data, sr = read_wav(tmp)
        finally:
            os.remove(tmp)
    else:
        data, sr = read_wav(src_bytes_or_path)

    data = resample_linear(data, sr)
    if data.size < TARGET_SR // 2:          # 短于 0.5 秒直接丢弃
        return False
    write_wav(dst_path, data)
    return True


# ---------------------------------------------------------------------------
# ESC-50
# ---------------------------------------------------------------------------

def do_esc50(dst, cache):
    zip_path = os.path.join(cache, "ESC-50-master.zip")
    if not os.path.exists(zip_path):
        print(f"下载 ESC-50（约 600MB）-> {zip_path}")
        os.makedirs(cache, exist_ok=True)
        with open(zip_path, "wb") as f:
            f.write(fetch(ESC50_ZIP, timeout=600))
    else:
        print(f"复用已下载的 {zip_path}")

    z = zipfile.ZipFile(zip_path)
    meta_name = [n for n in z.namelist() if n.endswith("meta/esc50.csv")][0]
    rows = list(csv.DictReader(io.StringIO(z.read(meta_name).decode())))
    print(f"ESC-50 元数据 {len(rows)} 条")

    counts = {}
    seq = {}
    for r in rows:
        target = ESC50_MAP.get(r["category"])
        if target is None:
            continue

        member = [n for n in z.namelist()
                  if n.endswith("audio/" + r["filename"])]
        if not member:
            continue

        outdir = os.path.join(dst, target)
        os.makedirs(outdir, exist_ok=True)

        # 来源分组用官方的 src_file（同一 Freesound 原始录音的切片共享它）
        source = f"esc50-{r['src_file']}"
        seq[source] = seq.get(source, 0)
        out = os.path.join(outdir, f"{source}__{seq[source]:03d}.wav")
        seq[source] += 1

        if convert(z.read(member[0]), out):
            counts[target] = counts.get(target, 0) + 1

    print("\nESC-50 映射结果：")
    for k in sorted(counts):
        print(f"  {k:12s} {counts[k]:5d} 条")
    print(f"\n许可证：CC BY-NC —— 请在 dataset_registry.csv 中署名 "
          f"Karol J. Piczak, ESC-50")
    return counts


# ---------------------------------------------------------------------------
# FSD50K
# ---------------------------------------------------------------------------

def fsd50k_small_files():
    """只下 ground_truth / doc / metadata 三个小文件（合计约 7MB）。"""
    rec = json.loads(fetch(ZENODO_FSD50K))
    links = {f["key"]: f["links"]["self"] for f in rec["files"]}
    out = {}
    for key in ("FSD50K.ground_truth.zip", "FSD50K.doc.zip",
                "FSD50K.metadata.zip"):
        size = next(f["size"] for f in rec["files"] if f["key"] == key)
        print(f"下载 {key}（{size / 1e6:.1f} MB）")
        out[key] = zipfile.ZipFile(io.BytesIO(fetch(links[key], timeout=300)))
    return out


def load_clip_licenses(meta_zip):
    """clip_id -> 许可证短名（cc0 / by / by-nc / sampling+ / other）。"""
    lic = {}
    for name in meta_zip.namelist():
        if not name.endswith("clips_info_FSD50K.json"):
            continue
        info = json.loads(meta_zip.read(name).decode())
        for cid, meta in info.items():
            url = (meta.get("license") or "").lower()
            if "publicdomain" in url or "zero" in url:
                short = "cc0"
            elif "by-nc" in url:
                short = "by-nc"
            elif "sampling" in url:
                short = "sampling+"
            elif "/by/" in url:
                short = "by"
            else:
                short = "other"
            lic[cid] = short
    return lic


def do_fsd50k_meta():
    zips = fsd50k_small_files()
    gt = zips["FSD50K.ground_truth.zip"]
    lic = load_clip_licenses(zips["FSD50K.metadata.zip"])

    per_class = {}
    for part in ("dev.csv", "eval.csv"):
        rows = list(csv.DictReader(io.StringIO(
            gt.read(f"FSD50K.ground_truth/{part}").decode())))
        for r in rows:
            labels = set(r["labels"].split(","))
            for lab in labels:
                target = FSD50K_MAP.get(lab)
                if target is None:
                    continue
                if labels & CONFLICT.get(target, set()):
                    continue
                key = (target, lab)
                per_class.setdefault(key, []).append(
                    (r["fname"], lic.get(r["fname"], "other")))

    print("\nFSD50K 可用 clip（已过滤冲突标签）：")
    print(f"{'目标类':<12}{'FSD50K 标签':<32}{'总数':>6}"
          f"{'cc0':>6}{'by':>6}{'by-nc':>7}{'其他':>6}")
    total_clean = set()
    for (target, lab), items in sorted(per_class.items()):
        c = {}
        for fname, l in items:
            c[l] = c.get(l, 0) + 1
            if l in ("cc0", "by"):
                total_clean.add(fname)
        print(f"{target:<12}{lab:<32}{len(items):>6}"
              f"{c.get('cc0', 0):>6}{c.get('by', 0):>6}"
              f"{c.get('by-nc', 0):>7}"
              f"{c.get('sampling+', 0) + c.get('other', 0):>6}")

    out = os.path.join(HERE, "reports", "fsd50k_candidates.csv")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["target_class", "fsd50k_label", "clip_id", "license"])
        for (target, lab), items in sorted(per_class.items()):
            for fname, l in items:
                w.writerow([target, lab, fname, l])

    print(f"\n候选清单已写出：{os.path.relpath(out, HERE)}")
    print(f"其中 CC0 + CC-BY（授权最干净）共 {len(total_clean)} 条 clip")
    print("\n下一步：下载 FSD50K.dev_audio / eval_audio 解压后执行")
    print("  python3 fetch_public_datasets.py --fsd50k-map <解压目录> --dst ../datasets")


def do_fsd50k_map(root, dst, licenses):
    cand = os.path.join(HERE, "reports", "fsd50k_candidates.csv")
    if not os.path.exists(cand):
        print("请先运行 --fsd50k-meta 生成候选清单", file=sys.stderr)
        return

    allow = {x.strip() for x in licenses.split(",")} if licenses else None
    rows = list(csv.DictReader(open(cand, encoding="utf-8")))

    # 建立 clip_id -> 实际文件路径（dev_audio / eval_audio 目录结构可能不同）
    index = {}
    for base, _, files in os.walk(root):
        for fn in files:
            if fn.endswith(".wav"):
                index[os.path.splitext(fn)[0]] = os.path.join(base, fn)

    print(f"在 {root} 下找到 {len(index)} 个 wav")

    counts = {}
    skipped_lic = 0
    for r in rows:
        if allow and r["license"] not in allow:
            skipped_lic += 1
            continue

        src = index.get(r["clip_id"])
        if src is None:
            continue

        target = r["target_class"]
        outdir = os.path.join(dst, target)
        os.makedirs(outdir, exist_ok=True)
        out = os.path.join(outdir, f"fsd50k-{r['clip_id']}__000.wav")
        if os.path.exists(out):
            continue
        if convert(src, out):
            counts[target] = counts.get(target, 0) + 1

    print("\nFSD50K 映射结果：")
    for k in sorted(counts):
        print(f"  {k:12s} {counts[k]:5d} 条")
    if skipped_lic:
        print(f"（按许可证过滤跳过 {skipped_lic} 条；"
              f"当前只接受 {licenses}）")
    print("\n提醒：用到的每条 clip 都要按其许可证署名，"
          "清单见 reports/fsd50k_candidates.csv")


def main():
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__)
    ap.add_argument("--esc50", action="store_true", help="下载并映射 ESC-50")
    ap.add_argument("--fsd50k-meta", action="store_true",
                    help="只下 FSD50K 元数据（7MB），输出候选清单与许可证分布")
    ap.add_argument("--fsd50k-map", metavar="DIR",
                    help="把本地已解压的 FSD50K 音频映射进来")
    ap.add_argument("--licenses", default="cc0,by",
                    help="映射时接受的许可证，逗号分隔；填 all 表示不过滤")
    ap.add_argument("--dst", default=os.path.join(HERE, "..", "datasets"))
    ap.add_argument("--cache", default=os.path.join(HERE, ".cache"))
    args = ap.parse_args()

    if not (args.esc50 or args.fsd50k_meta or args.fsd50k_map):
        ap.print_help()
        return 1

    dst = os.path.abspath(args.dst)

    if args.esc50:
        do_esc50(dst, os.path.abspath(args.cache))

    if args.fsd50k_meta:
        do_fsd50k_meta()

    if args.fsd50k_map:
        do_fsd50k_map(os.path.abspath(args.fsd50k_map), dst,
                      None if args.licenses == "all" else args.licenses)

    if args.esc50 or args.fsd50k_map:
        print(f"\n数据目录：{dst}")
        print("下一步：python3 train_models.py --data-dir "
              f"{os.path.relpath(dst, HERE)}")
        print("注意：呻吟(moan) 与 姓名/方言求救词 无公开数据，必须自采，"
              "见 dataset_sources.md 第四节")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
