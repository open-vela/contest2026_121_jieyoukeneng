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

  # 2) FSD50K 元数据（仅 7MB）：看清单与许可证分布，再决定要下哪些 clip
  python3 fetch_public_datasets.py --fsd50k-meta

  # 3) 按清单逐 clip 下载（推荐）：只取需要的约 8.5k 条，不碰 Zenodo 的 25GB 分卷
  python3 fetch_public_datasets.py --fsd50k-fetch --dst ../datasets

  # 4) 或者已经把 FSD50K 官方分卷解压到本地了，从本地映射进来
  python3 fetch_public_datasets.py --fsd50k-map /path/to/FSD50K --dst ../datasets

  # 只保留授权最干净的 clip（默认只要 CC0 与 CC-BY）
  python3 fetch_public_datasets.py --fsd50k-fetch --licenses cc0,by --dst ../datasets

为什么不下 Zenodo 分卷
----------------------
Zenodo 上 FSD50K 音频是 6 个分卷压缩包（dev 18.4GB + eval 6.3GB），必须全部
下齐才能合并解压，解压后峰值占盘翻倍，而我们只需要其中约 17% 的 clip。
HuggingFace 镜像把 clip 逐个平铺存放，可以按 ID 精确取用、断点续传、落盘即
最终形态。Zenodo 仍用于取 7MB 元数据（--fsd50k-meta），那部分不可替代。

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
import threading
import time
import urllib.error
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

# FSD50K 逐 clip 镜像：把官方分卷压缩包平铺成独立 wav，可按 ID 精确取用。
# 境外直连不通时用 hf-mirror；设 HF_ENDPOINT 可切回 huggingface.co。
HF_ENDPOINT = os.environ.get("HF_ENDPOINT", "https://hf-mirror.com").rstrip("/")
FSD50K_CLIPS = HF_ENDPOINT + "/datasets/Fhrozen/FSD50k/resolve/main/clips"

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
    print("\n下一步：按清单逐 clip 下载（约 5.5GB，不需要 Zenodo 的 25GB 分卷）")
    print("  python3 fetch_public_datasets.py --fsd50k-fetch --dst ../datasets")


def load_candidates(licenses):
    """读候选清单 -> {clip_id: target_class}。

    同一 clip 可能带多个我们关心的标签：若都映射到同一目标类（如 Glass 与
    Shatter 都是 impact）只保留一份；若映射到不同目标类（如既是 Water 又是
    Knock）则整条丢弃——CONFLICT 表只挡了标签层面的混杂，跨目标类的这 192 条
    必须在这里挡掉，否则同一段音频会同时成为两个类的正样本。
    """
    cand = os.path.join(HERE, "reports", "fsd50k_candidates.csv")
    if not os.path.exists(cand):
        raise RuntimeError("请先运行 --fsd50k-meta 生成候选清单")

    allow = {x.strip() for x in licenses.split(",")} if licenses else None
    targets = {}
    lic_of = {}
    skipped_lic = 0
    for r in csv.DictReader(open(cand, encoding="utf-8")):
        if allow and r["license"] not in allow:
            skipped_lic += 1
            continue
        targets.setdefault(r["clip_id"], set()).add(r["target_class"])
        lic_of[r["clip_id"]] = r["license"]

    keep = {cid: next(iter(t)) for cid, t in targets.items() if len(t) == 1}
    conflict = len(targets) - len(keep)
    return keep, lic_of, skipped_lic, conflict


def fsd50k_clip_url(clip_id, split):
    return f"{FSD50K_CLIPS}/{split}/{clip_id}.wav"


class _Redirect308(urllib.request.HTTPRedirectHandler):
    """Python 3.10 及更早的 urllib 不认 308，而 HF 镜像正是用 308 跳到 CDN。"""

    def http_error_308(self, req, fp, code, msg, headers):
        return self.http_error_307(req, fp, 307, msg, headers)


_OPENER = urllib.request.build_opener(_Redirect308)


def fetch_clip(clip_id, timeout=60, tries=3):
    """按 clip_id 取音频。清单里没记 dev/eval 归属，先试 dev 再回退 eval。"""
    err = None
    for attempt in range(tries):
        for split in ("dev", "eval"):
            try:
                with _OPENER.open(
                        fsd50k_clip_url(clip_id, split), timeout=timeout) as r:
                    return r.read(), split
            except urllib.error.HTTPError as exc:
                if exc.code == 404:          # 不在这个 split，换一个
                    err = exc
                    continue
                err = exc
            except Exception as exc:         # noqa: BLE001
                err = exc
        if attempt + 1 < tries:
            time.sleep(2 * (attempt + 1))
    raise RuntimeError(f"clip {clip_id} 取回失败: {err}")


def do_fsd50k_fetch(dst, licenses, workers, limit):
    keep, lic_of, skipped_lic, conflict = load_candidates(licenses)

    todo = []
    have = 0
    for clip_id, target in sorted(keep.items()):
        out = os.path.join(dst, target, f"fsd50k-{clip_id}__000.wav")
        if os.path.exists(out):
            have += 1
            continue
        todo.append((clip_id, target, out))

    if limit:
        todo = todo[:limit]

    print(f"候选 clip {len(keep)} 条（许可证过滤跳过 {skipped_lic} 行，"
          f"跨目标类冲突丢弃 {conflict} 条）")
    print(f"已在本地 {have} 条，本次待下载 {len(todo)} 条，并发 {workers}")
    if not todo:
        print("没有需要下载的 clip")
        return

    for target in sorted({t for _, t, _ in todo}):
        os.makedirs(os.path.join(dst, target), exist_ok=True)

    lock = threading.Lock()
    counts = {}
    splits = {}
    failed = []
    too_short = []
    done = [0]
    t0 = time.time()

    def work(item):
        clip_id, target, out = item
        try:
            raw, split = fetch_clip(clip_id)
        except Exception as exc:                        # noqa: BLE001
            with lock:
                failed.append((clip_id, str(exc)))
                done[0] += 1
            return

        # 先落到 .part 再改名，中途中断不会留下半个 wav 让续传误判为已完成
        part = out + ".part"
        ok = convert(raw, part)                         # -> 16kHz 单声道
        if ok:
            os.replace(part, out)
        with lock:
            done[0] += 1
            if ok:
                counts[target] = counts.get(target, 0) + 1
                splits[split] = splits.get(split, 0) + 1
            else:
                too_short.append(clip_id)
            n = done[0]
            if n % 200 == 0 or n == len(todo):
                el = time.time() - t0
                rate = n / el if el > 0 else 0
                eta = (len(todo) - n) / rate if rate > 0 else 0
                print(f"  {n}/{len(todo)}  {rate:.1f} clip/s  "
                      f"剩余约 {eta / 60:.1f} 分钟  失败 {len(failed)}")

    # 逐 clip 下载是网络 IO 密集型，线程池即可；并发过高会被镜像限速
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=workers) as pool:
        list(pool.map(work, todo))

    print("\nFSD50K 下载结果：")
    for k in sorted(counts):
        print(f"  {k:12s} {counts[k]:5d} 条")
    print(f"  来源切分：dev {splits.get('dev', 0)} / eval {splits.get('eval', 0)}")
    if too_short:
        print(f"（{len(too_short)} 条短于 0.5 秒已丢弃）")
    if failed:
        print(f"\n失败 {len(failed)} 条，重跑本命令即可续传（已下载的会跳过）：")
        for cid, msg in failed[:10]:
            print(f"  {cid}: {msg}")
        if len(failed) > 10:
            print(f"  ... 另有 {len(failed) - 10} 条")
    print(f"\n耗时 {(time.time() - t0) / 60:.1f} 分钟")
    print("提醒：用到的每条 clip 都要按其许可证署名，"
          "清单见 reports/fsd50k_candidates.csv")


def do_fsd50k_map(root, dst, licenses):
    try:
        keep, _, skipped_lic, conflict = load_candidates(licenses)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return

    # 建立 clip_id -> 实际文件路径（dev_audio / eval_audio 目录结构可能不同）
    index = {}
    for base, _, files in os.walk(root):
        for fn in files:
            if fn.endswith(".wav"):
                index[os.path.splitext(fn)[0]] = os.path.join(base, fn)

    print(f"在 {root} 下找到 {len(index)} 个 wav")

    counts = {}
    for clip_id, target in sorted(keep.items()):
        src = index.get(clip_id)
        if src is None:
            continue

        outdir = os.path.join(dst, target)
        os.makedirs(outdir, exist_ok=True)
        out = os.path.join(outdir, f"fsd50k-{clip_id}__000.wav")
        if os.path.exists(out):
            continue
        if convert(src, out):
            counts[target] = counts.get(target, 0) + 1

    print("\nFSD50K 映射结果：")
    for k in sorted(counts):
        print(f"  {k:12s} {counts[k]:5d} 条")
    if skipped_lic:
        print(f"（按许可证过滤跳过 {skipped_lic} 行；"
              f"当前只接受 {licenses}）")
    if conflict:
        print(f"（跨目标类冲突丢弃 {conflict} 条）")
    print("\n提醒：用到的每条 clip 都要按其许可证署名，"
          "清单见 reports/fsd50k_candidates.csv")


def main():
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__)
    ap.add_argument("--esc50", action="store_true", help="下载并映射 ESC-50")
    ap.add_argument("--fsd50k-meta", action="store_true",
                    help="只下 FSD50K 元数据（7MB），输出候选清单与许可证分布")
    ap.add_argument("--fsd50k-fetch", action="store_true",
                    help="按候选清单从 HF 镜像逐 clip 下载（推荐，约 5.5GB）")
    ap.add_argument("--fsd50k-map", metavar="DIR",
                    help="把本地已解压的 FSD50K 官方分卷映射进来")
    ap.add_argument("--workers", type=int, default=16,
                    help="逐 clip 下载的并发数（默认 16）")
    ap.add_argument("--limit", type=int, default=0,
                    help="只下前 N 条，用于先小跑一段确认链路")
    ap.add_argument("--licenses", default="cc0,by",
                    help="接受的许可证，逗号分隔；填 all 表示不过滤")
    ap.add_argument("--dst", default=os.path.join(HERE, "..", "datasets"))
    ap.add_argument("--cache", default=os.path.join(HERE, ".cache"))
    args = ap.parse_args()

    if not (args.esc50 or args.fsd50k_meta or args.fsd50k_fetch
            or args.fsd50k_map):
        ap.print_help()
        return 1

    dst = os.path.abspath(args.dst)
    licenses = None if args.licenses == "all" else args.licenses

    if args.esc50:
        do_esc50(dst, os.path.abspath(args.cache))

    if args.fsd50k_meta:
        do_fsd50k_meta()

    if args.fsd50k_fetch:
        do_fsd50k_fetch(dst, licenses, args.workers, args.limit)

    if args.fsd50k_map:
        do_fsd50k_map(os.path.abspath(args.fsd50k_map), dst, licenses)

    if args.esc50 or args.fsd50k_fetch or args.fsd50k_map:
        print(f"\n数据目录：{dst}")
        print("下一步：python3 train_models.py --data-dir "
              f"{os.path.relpath(dst, HERE)}")
        print("注意：姓名/方言求救短语走端侧 enroll 现场录入（vg_enroll），"
              "不需要训练数据；见 dataset_sources.md 第四节")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
