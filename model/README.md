# model/ —— 数据、训练、量化与端侧部署 (PRD-07)

本目录负责「数据 → 特征 → 训练 → int8 量化 → 端侧 C 权重」的完整链路。
产物 `vg_model_weights.c` 直接编入固件，端侧不引入任何第三方推理运行时。

## 目录

| 文件 | 作用 |
|---|---|
| `velaguard_features.py` | 特征提取，**与端侧 `vg_feature.c` 逐步骤对齐** |
| `synth_dataset.py` | 自举合成信号数据集（无版权风险、可复现） |
| `fetch_public_datasets.py` | **公开数据集下载与映射**（ESC-50 / FSD50K → 我们的七类目录） |
| `map_new_datasets.py` | **第二批公开数据映射**（Nonspeech7k / VIVAE / ASVP-ESD / VocalSound / AudioSet，从 `datasets/_raw/` 映射进七类目录） |
| `prepare_dataset.py` | 真实音频重采样 / 切片 / 目录整理 |
| `train_models.py` | 训练、评估、int8 量化、导出端侧权重与指标报告 |
| `dataset_sources.md` | **公开数据集调研**：类别映射、许可证、数量、缺口（已实际核对） |
| `dataset_registry.csv` | **数据来源与许可证登记表**（PRD-07 要求随 model/ 提交） |
| `reports/` | 自动生成的指标报告与 FSD50K 候选清单 |

## 快速开始（自举模式，无需任何数据）

```bash
pip3 install numpy
cd model
python3 train_models.py            # 约 30 秒
```

产出：

- `../app/velaguard_device/audio_detector/vg_model_weights.c` —— 端侧权重（约 4.5KB）
- `reports/metrics.md` —— 指标报告

> ⚠️ **自举模式的指标不是 PRD-07 验收指标。** 合成信号类别间人为可分，
> F1 会接近 1.0，它只证明「特征 → 训练 → 量化 → 端侧 C 推理」链路已打通，
> 并提供一份可编译可运行的初始权重。正式指标必须用下面的真实数据流程复现。

## 正式流程（真实数据）

### 1. 收集六组数据

**先看 [`dataset_sources.md`](dataset_sources.md)** —— 里面有已实际核对过的
公开数据集调研结论（类别映射、clip 数量、逐条许可证分布）。

一条命令拿到 ESC-50 并映射成训练目录：

```bash
python3 fetch_public_datasets.py --esc50 --dst ../datasets
```

FSD50K 先只下 7MB 元数据，再按清单从 HF 镜像逐 clip 精确下载
（不需要 Zenodo 的 25GB 分卷；已于 2026-08-02 实跑，30.7 分钟入库 8267 条）：

```bash
python3 fetch_public_datasets.py --fsd50k-meta
# 输出 reports/fsd50k_candidates.csv：11963 条可用 clip，
# 其中 CC0 + CC-BY 共 8743 条（授权最干净）
python3 fetch_public_datasets.py --fsd50k-fetch --dst ../datasets
```

| 组 | 公开数据 | 必须自采 |
|---|---|---|
| 报警声 | FSD50K 1584 + AudioSet-bal 525 条（**含真实烟感 Smoke detector 54 + Fire alarm 55**）+ ESC-50 80 条 | 真实烟感实响补充（演示核心场景） |
| 水流声 | FSD50K 2401 + AudioSet-bal 511 条 + ESC-50 120 条 | 自家水龙头/淋浴（可选） |
| 破碎撞击 | FSD50K 1644 + AudioSet-bal 307 条 + ESC-50 80 条 | 安全替代物（可选） |
| 痛苦叫声/哭喊 | FSD50K 384 + Nonspeech7k 2659 + ASVP-ESD 1144 + VIVAE fear 139 + AudioSet 57 + ESC-50 40 | 可选 |
| 连续呼救 | FSD50K `Shout`+`Yell` 229 + VIVAE anger 157 + AudioSet 10（喊叫声学纹理与语种无关；中文短语走端侧 enroll 录入，见下） | 中文求救词由 PRD-02 模板通道现场录入，**不需要训练数据** |
| 呻吟 | **VIVAE pain 166 + ASVP-ESD pain/groan 778 + AudioSet 61（2026-07 起已有公开打底）** | 仍需自采定型（贴近真实距离/强度） |
| 姓名/方言 | 天然无公开数据 | **必采**，PRD-02 最小集 |
| 背景负样本 | FSD50K 2025 + ASVP 3973 + VocalSound 3000（限量导入）+ Nonspeech7k 4355 + ESC-50 640 | 自家电视/交谈/做饭/**平静喊名** |

第二批五个数据源的下载渠道（本机境外直连不通，全部走 hf-mirror / Kaggle
匿名直连）、许可证核对与实测数量，见 `dataset_sources.md` 第七节；
映射命令：

```bash
python3 map_new_datasets.py --all                      # 全部导入
python3 map_new_datasets.py --vocalsound --limit 3000  # 背景类限量，防止淹没正样本
```

每用一条公开数据都要在 `dataset_registry.csv` 登记来源、许可证与署名要求。

### 2. 预处理

```bash
python3 prepare_dataset.py --src /path/to/raw_audio --dst ../datasets
```

原始音频与 `datasets/` **不进 Git**（`.gitignore` 已排除），仓库只保留来源说明与脚本。

### 3. 训练与导出

```bash
python3 train_models.py --data-dir ../datasets --epochs 3000 --features v2 --final
```

> `--features v2`：40 维 MFCC 统计 + 13 维瞬态/时序/谱形描述子（2026-08
> 定稿）。40 维统计特征把窗内时间轴压扁，表达不了 impact/alarm 的瞬态
> 结构与 moan/scream 的音色亮度差——数据量 x8.8、波形增广、类别平衡三个
> 实验都突破不了这个表征瓶颈，v2 一次解决（impact 事件级 F1 0.46 → 0.76）。
>
> `--final`：定稿结构 = env 四类 + voice 二分类（moan/scream/shout_help
> 合并为 distress，三类在状态机里的处置本就同路，且两两互混恰是错误主体：
> 四类事件级宏 F1 仅 0.54，合并后 0.89）。
>
> 需要覆盖 `tests/` 合成回归夹具与 `velaguard sim` 演示信号时可加
> `--mix-synth 60`（每类混入 60 条合成 clip；混入后指标表的测试集含合成
> 分组，验收对照以纯真实域数字为准）。

脚本会：

1. 按 1 秒窗口 / 50% 重叠切片提特征（多进程并行）；正样本类做
   **弱标签能量过滤**（只保留能量在该 clip 峰值 12dB 以内的窗口，清除
   长音频里"标着事件实为静音"的脏窗口）；
2. **按录音来源分组切分**训练/测试集（同一原始录音/同一说话人不跨集，
   PRD-07 硬性要求）；
3. 训练环境声景（4 类）与人声异常（2 类）两个 MLP，类别不平衡用
   **sqrt 回火的逆频率权重**（完全逆频率实测会让极少数类精确率崩溃），
   学习率按 50% / 80% 进度阶梯衰减；
4. 同时评估 float 与 int8 量化、**窗口级与事件级**（按原始录音软投票，
   与 PRD-08 实机验收的"触发成功率"同口径）两组指标；
5. 导出 `vg_model_weights.c` 并写出 `reports/metrics.md`。

实验用开关：`--merge-voice`（在四类基础上额外训练二分类对照）、
`--augment-to N` / `--balance R`（波形增广与训练集平衡；对本任务实测
无效甚至有害——增广的混响/变速会抹掉瞬态类的时域结构，结论保留在
`reports/metrics_step3_event_merge.md` 之前的归档里）、`--workers N`。

### 4. 验收线（PRD-07）

- 宏平均 F1 ≥ 0.80，每个安全类别召回率 ≥ 0.80
  —— **以事件级口径为主**（按原始录音软投票；产品链路里单窗口误判会被
  检测器的多窗口聚合滤掉，PRD-08 实机验收的"每类 ≥20 次触发、成功率
  ≥80%"本来就是事件级）。窗口级指标在报告中如实并列。
- 个性化求救短语安静环境召回率 ≥ 0.85（由 `enrollment` 模板匹配负责，
  用 `velaguard enroll add` 录入后在实机复测）

### 5. 一致性校验（必做）

电脑端训练用的特征与端侧 C 实现必须逐维一致，否则训练指标在板上不成立：

```bash
cd ../tests && make && python3 check_feature_parity.py
```

当前实测最大绝对误差 **3.0e-05**（阈值 2e-3）。

## 模型结构与预算

```
16kHz 单声道 1s 窗口（50% 重叠）
  → 预加重 0.97 → Hamming 25ms/10ms → 512 点 FFT
  → 26 路 Mel(20~7800Hz) → log → DCT → 13 维 MFCC
  → 统计聚合：均值(13) + 标准差(13) + 一阶差分均值(13) + 过零率(1)
  → v2 追加（同一遍 FFT 的副产品，13 维）：
      帧能量峰比 / 最大起落沿 / 高能帧占比        （瞬态结构）
      4 段能量轮廓                                 （粗粒度时序）
      谱质心均值/标准差/帧间变化、>1k/>4kHz 占比    （音色亮度）
  = 53 维
  → 两层 MLP（int8 权重 + float 偏置 + ReLU + Softmax）
      环境声景：53 → 48 → 4   (other / alarm_beep / water_flow / impact)
      人声异常：53 → 32 → 2   (none / distress)
```

呻吟/尖叫的急促程度区分不再依赖分类器细分，由检测器
（`vg_detector.c: vg_estimate_urgency`）用谱质心+能量+过零率估计——
状态机的分级升级行为不变。

| 预算项 | PRD-07 上限 | 当前 |
|---|---|---|
| 两个模型权重合计 | ≤ 1MB | **5.7KB** |
| 关键词/个性化模板 | ≤ 200KB | 16 条 × 176B ≈ **2.8KB** |
| 闪存总增量 | ≤ 2MB | 权重 + 特征代码 ≈ **60KB** |
| 单窗口处理耗时 | < 500ms | 主机侧 ~2ms，实机待标定（`velaguard status` 输出） |

## 为什么是 MLP 而不是 DS-CNN

PRD-07 的降级决断表写明：「板端量化推理示例接入 2 个工作日内未跑通 →
降级为 MFCC + 小型 MLP/线性分类器，权重导出 C 数组，不引入新推理框架」。

当前 openvela 工程内没有可直接在 Gemini-S1 上编译运行的量化推理示例，
且开发板未到手，因此**直接落地降级路线**：纯 C 前向、零第三方依赖、
体积与耗时都远低于预算，同时保留了升级到 DS-CNN 的空间——
只要替换 `vg_classifier.c` 的前向实现，特征与接口都不需要改。
