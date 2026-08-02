#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安聆 VelaGuard - 第二批公开数据集映射（datasets/_raw -> datasets/<类别>）

处理 2026-07-31 下载的五个数据源（调研与实点数量见 dataset_sources.md 第七节，
许可证登记见 dataset_registry.csv）：

  nonspeech7k  parquet(wav字节)  screaming/crying -> scream，其余人声 -> background
  vivae        arrow(wav字节)    pain -> moan，fear -> scream，anger -> shout_help
  asvp         磁盘 wav          情绪码 11 -> moan，06/04 -> scream，01/02/03/13 -> background
  vocalsound   磁盘 wav          六类全部 -> background（防 moan 误报的负样本）
  audioset     parquet(flac字节) 按 human_labels 映射七类，带冲突标签过滤

输出遵循 train_models.py 约定：16kHz/16bit/单声道，文件名 `<source>__<seq>.wav`，
`__` 前缀是分组键（同一原始录音/同一说话人不跨训练测试集）：

  ns7k-<freesound源ID>   同一原始录音的切片共享前缀（如 108160-1/-2 同源）
  vivae-f<行号>          镜像丢失了原始文件名，无法恢复说话人，只能按 clip 分组
                         （11 名说话人可能跨集，评估指标会略偏乐观，报告时注明）
  asvp-a<演员ID>         按演员分组（文件名第 6 段）
  vsnd-<说话人ID>        按说话人分组（文件名首段）
  aset-<video_id>        每条 YouTube 视频一组

用法
----
  python3 map_new_datasets.py --all --raw ../datasets/_raw --dst ../datasets
  # 或单独导入，并给背景类设上限防止淹没正样本：
  python3 map_new_datasets.py --vocalsound --limit 3000
"""

import argparse
import csv
import io
import os
import re
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from prepare_dataset import read_wav, resample_linear, write_wav, TARGET_SR  # noqa: E402

MIN_SAMPLES = TARGET_SR // 2        # 与 fetch_public_datasets.py 一致：<0.5s 丢弃

NS7K_MAP = {
    "screaming": "scream",
    "crying": "scream",
    "breath": "background",
    "cough": "background",
    "laugh": "background",
    "sneeze": "background",
    "yawn": "background",
    "yawm": "background",           # 官方 test 集里的拼写错误
}

VIVAE_MAP = {
    "pain": "moan",
    "fear": "scream",
    "anger": "shout_help",
    # achievement / pleasure / surprise 刻意不导入：
    # pleasure 的发声与 moan 声学上难分，放进 background 会污染负样本
}

ASVP_EMOTION_MAP = {
    "11": "moan",                   # pain, groan
    "06": "scream",                 # fearful, scream, panic
    # 04 sad/cry 不再导入 scream：哭泣与尖叫声学差异大，首轮实测把 scream
    # 测试窗口撑到 1.65 万个并显著拉低精确率（2026-08-01 移除）
    "01": "background",             # boredom, sigh
    "02": "background",             # neutral, calm
    "03": "background",             # happy, laugh
    "13": "background",             # breath
    # 05 angry/grunt 不导入：多为愤怒语音而非呼喊，归 shout_help 会引入噪声
}

AUDIOSET_MAP = {
    "Smoke detector, smoke alarm": "alarm_beep",
    "Fire alarm": "alarm_beep",
    "Alarm": "alarm_beep",
    "Alarm clock": "alarm_beep",
    "Buzzer": "alarm_beep",
    "Beep, bleep": "alarm_beep",
    "Doorbell": "alarm_beep",
    "Siren": "alarm_beep",
    "Shout": "shout_help",
    "Yell": "shout_help",
    "Groan": "moan",
    "Wail, moan": "moan",
    "Whimper": "moan",
    "Screaming": "scream",
    "Crying, sobbing": "scream",
    "Baby cry, infant cry": "scream",
    "Glass": "impact",
    "Shatter": "impact",
    "Smash, crash": "impact",
    "Thump, thud": "impact",
    "Knock": "impact",
    "Slam": "impact",
    "Water": "water_flow",
    "Water tap, faucet": "water_flow",
    "Sink (filling or washing)": "water_flow",
    "Bathtub (filling or washing)": "water_flow",
    "Toilet flush": "water_flow",
    "Boiling": "water_flow",
}

# 与 fetch_public_datasets.py 的 CONFLICT 同思路：环境类混入语音/音乐的 clip 跳过
AUDIOSET_CONFLICT = {
    "alarm_beep": {"Speech", "Music"},
    "water_flow": {"Speech", "Music"},
    "impact": {"Speech", "Music"},
    "scream": {"Music"},
    "shout_help": {"Music"},
    "moan": {"Music"},
}


class Writer:
    """统一落盘：限量、去重、计数。"""

    def __init__(self, dst, limit):
        self.dst = dst
        self.limit = limit
        self.counts = {}
        self.skipped = 0
        self.errors = 0
        self.seq = {}

    def _out_path(self, target, source):
        outdir = os.path.join(self.dst, target)
        os.makedirs(outdir, exist_ok=True)
        n = self.seq.get(source, 0)
        self.seq[source] = n + 1
        return os.path.join(outdir, f"{source}__{n:03d}.wav")

    def put(self, target, source, data, sr):
        if self.limit and self.counts.get(target, 0) >= self.limit:
            self.skipped += 1
            return
        out = self._out_path(target, source)
        if os.path.exists(out):
            return
        try:
            pcm = resample_linear(data, sr)
            if pcm.size < MIN_SAMPLES:
                self.skipped += 1
                return
            write_wav(out, pcm)
            self.counts[target] = self.counts.get(target, 0) + 1
        except Exception:                                   # noqa: BLE001
            self.errors += 1

    def put_wav_bytes(self, target, source, blob):
        tmp = os.path.join(self.dst, ".tmp_convert.wav")
        try:
            with open(tmp, "wb") as f:
                f.write(blob)
            data, sr = read_wav(tmp)
        except Exception:                                   # noqa: BLE001
            self.errors += 1
            return
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
        self.put(target, source, data, sr)

    def put_wav_file(self, target, source, path):
        try:
            data, sr = read_wav(path)
        except Exception:                                   # noqa: BLE001
            self.errors += 1
            return
        self.put(target, source, data, sr)

    def report(self, name):
        print(f"\n{name} 映射结果：")
        for k in sorted(self.counts):
            print(f"  {k:12s} {self.counts[k]:5d} 条")
        if self.skipped:
            print(f"  （限量/过短跳过 {self.skipped} 条）")
        if self.errors:
            print(f"  （读取失败 {self.errors} 条）")


# ---------------------------------------------------------------------------

def do_nonspeech7k(raw, dst, limit):
    import pyarrow.parquet as pq
    w = Writer(dst, limit)
    for split in ("train", "test"):
        path = os.path.join(raw, "nonspeech7k", f"{split}.parquet")
        t = pq.read_table(path, columns=["audio", "filename", "classname"])
        for audio, fname, cls in zip(t.column("audio").to_pylist(),
                                     t.column("filename").to_pylist(),
                                     t.column("classname").to_pylist()):
            target = NS7K_MAP.get(cls)
            if target is None:
                continue
            m = re.match(r"^\d+", fname)
            group = m.group(0) if m else os.path.splitext(fname)[0]
            w.put_wav_bytes(target, f"ns7k-{group}", audio["bytes"])
    w.report("Nonspeech7k")
    return w


def do_vivae(raw, dst, limit):
    import glob

    import pyarrow as pa
    w = Writer(dst, limit)
    for path in glob.glob(os.path.join(raw, "vivae", "data", "*.arrow")):
        try:
            t = pa.ipc.open_stream(open(path, "rb")).read_all()
        except pa.lib.ArrowInvalid:
            t = pa.ipc.open_file(path).read_all()
        for i, (subset, label, audio) in enumerate(zip(
                t.column("set").to_pylist(),
                t.column("label").to_pylist(),
                t.column("audio").to_pylist())):
            if subset != "full_set":        # core_set 是 full_set 的子集，避免重复
                continue
            target = VIVAE_MAP.get(label)
            if target is None:
                continue
            w.put_wav_bytes(target, f"vivae-f{i}", audio["bytes"])
    w.report("VIVAE")
    return w


def do_asvp(raw, dst, limit):
    w = Writer(dst, limit)
    root = os.path.join(raw, "asvp_esd", "ASVP-ESD-Update", "Audio")
    for base, _, files in os.walk(root):
        for fn in sorted(files):
            if not fn.endswith(".wav"):
                continue
            fields = os.path.splitext(fn)[0].split("-")
            if len(fields) < 6:
                continue
            target = ASVP_EMOTION_MAP.get(fields[2])
            if target is None:
                continue
            w.put_wav_file(target, f"asvp-a{fields[5]}",
                           os.path.join(base, fn))
    w.report("ASVP-ESD")
    return w


def do_vocalsound(raw, dst, limit):
    w = Writer(dst, limit)
    root = os.path.join(raw, "vocalsound", "VocalSound")
    for part in ("train.csv", "test.csv"):
        with open(os.path.join(root, part), encoding="utf-8") as f:
            for row in csv.DictReader(f):
                fn = os.path.basename(row["path"])
                speaker = fn.split("_")[0]
                w.put_wav_file("background", f"vsnd-{speaker}",
                               os.path.join(root, row["path"]))
    w.report("VocalSound")
    return w


def do_audioset(raw, dst, limit):
    import glob

    import pyarrow.parquet as pq
    import soundfile as sf
    w = Writer(dst, limit)
    shards = sorted(glob.glob(
        os.path.join(raw, "audioset_bal", "data", "bal_train", "*.parquet")))
    for shard in shards:
        pf = pq.ParquetFile(shard)
        for batch in pf.iter_batches(batch_size=64,
                                     columns=["video_id", "audio",
                                              "human_labels"]):
            for vid, audio, labels in zip(
                    batch.column("video_id").to_pylist(),
                    batch.column("audio").to_pylist(),
                    batch.column("human_labels").to_pylist()):
                labels = set(labels)
                targets = {AUDIOSET_MAP[l] for l in labels if l in AUDIOSET_MAP}
                if len(targets) != 1:       # 无目标或跨类混杂，跳过
                    continue
                target = targets.pop()
                if labels & AUDIOSET_CONFLICT.get(target, set()):
                    continue
                try:
                    data, sr = sf.read(io.BytesIO(audio["bytes"]),
                                       dtype="float64")
                except Exception:                           # noqa: BLE001
                    w.errors += 1
                    continue
                if data.ndim > 1:
                    data = data.mean(axis=1)
                w.put(target, f"aset-{vid}", data * 32767.0, sr)
        print(f"  已处理 {os.path.basename(shard)}")
    w.report("AudioSet balanced")
    return w


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__)
    ap.add_argument("--nonspeech7k", action="store_true")
    ap.add_argument("--vivae", action="store_true")
    ap.add_argument("--asvp", action="store_true")
    ap.add_argument("--vocalsound", action="store_true")
    ap.add_argument("--audioset", action="store_true")
    ap.add_argument("--all", action="store_true", help="导入全部五个数据源")
    ap.add_argument("--limit", type=int, default=0,
                    help="每个目标类别最多导入多少条（0 = 不限），"
                         "对本次命令启用的每个数据源分别计数")
    ap.add_argument("--raw", default=os.path.join(HERE, "..", "datasets", "_raw"))
    ap.add_argument("--dst", default=os.path.join(HERE, "..", "datasets"))
    args = ap.parse_args()

    jobs = {
        "nonspeech7k": do_nonspeech7k,
        "vivae": do_vivae,
        "asvp": do_asvp,
        "vocalsound": do_vocalsound,
        "audioset": do_audioset,
    }
    picked = [k for k in jobs if args.all or getattr(args, k)]
    if not picked:
        ap.print_help()
        return 1

    raw = os.path.abspath(args.raw)
    dst = os.path.abspath(args.dst)
    total = {}
    for name in picked:
        w = jobs[name](raw, dst, args.limit)
        for k, v in w.counts.items():
            total[k] = total.get(k, 0) + v

    print("\n本次合计新增：")
    for k in sorted(total):
        print(f"  {k:12s} {total[k]:5d} 条")
    print(f"\n数据目录：{dst}")
    print("下一步：python3 train_models.py --data-dir "
          f"{os.path.relpath(dst, HERE)} --epochs 1500")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
