# model/ —— 数据、训练、量化与端侧部署 (PRD-07)

本目录负责「数据 → 特征 → 训练 → int8 量化 → 端侧 C 权重」的完整链路。
产物 `vg_model_weights.c` 直接编入固件，端侧不引入任何第三方推理运行时。

## 目录

| 文件 | 作用 |
|---|---|
| `velaguard_features.py` | 特征提取，**与端侧 `vg_feature.c` 逐步骤对齐** |
| `synth_dataset.py` | 自举合成信号数据集（无版权风险、可复现） |
| `prepare_dataset.py` | 真实音频重采样 / 切片 / 目录整理 |
| `train_models.py` | 训练、评估、int8 量化、导出端侧权重与指标报告 |
| `dataset_registry.csv` | **数据来源与许可证登记表**（PRD-07 要求随 model/ 提交） |
| `reports/` | 自动生成的指标报告（`metrics.md` / `metrics.json`） |

## 快速开始（自举模式，无需任何数据）

```bash
pip3 install numpy
cd model
python3 train_models.py            # 约 30 秒
```

产出：

- `../app/velaguard_device/audio_detector/vg_model_weights.c` —— 端侧权重（约 2.6KB）
- `reports/metrics.md` —— 指标报告

> ⚠️ **自举模式的指标不是 PRD-07 验收指标。** 合成信号类别间人为可分，
> F1 会接近 1.0，它只证明「特征 → 训练 → 量化 → 端侧 C 推理」链路已打通，
> 并提供一份可编译可运行的初始权重。正式指标必须用下面的真实数据流程复现。

## 正式流程（真实数据）

### 1. 收集六组数据

| 组 | 来源建议 |
|---|---|
| 报警声 | ESC-50 `siren`/`clock_alarm`、FSD50K `Alarm/Smoke detector/Buzzer` + 自录真实烟感 |
| 水流声 | ESC-50 `pouring_water`/`water_drops`、FSD50K `Water tap` + 自录水龙头/淋浴/漏水 |
| 破碎撞击 | ESC-50 `glass_breaking`、FSD50K `Glass/Shatter/Thump` + 自录安全替代物 |
| 呻吟/痛苦叫声 | **公开数据稀缺，以自采拟音为主**（须获录制者书面同意） |
| 姓名/求救词 | 全部自录，平静与急促**成对**录制（PRD-02 最小集） |
| 背景负样本 | ESC-50/FSD50K 背景类 + 自录电视声、交谈、做饭、**平静喊名** |

每用一条公开数据都要在 `dataset_registry.csv` 登记来源、许可证与署名要求。
ESC-50 / UrbanSound8K 为 CC BY-NC 系；FSD50K 逐 clip 授权，必须逐条核对。

### 2. 预处理

```bash
python3 prepare_dataset.py --src /path/to/raw_audio --dst ../datasets
```

原始音频与 `datasets/` **不进 Git**（`.gitignore` 已排除），仓库只保留来源说明与脚本。

### 3. 训练与导出

```bash
python3 train_models.py --data-dir ../datasets --epochs 1200
```

脚本会：

1. 按 1 秒窗口 / 50% 重叠切片提特征；
2. **按录音来源分组切分**训练/测试集（同一原始录音不跨集，PRD-07 硬性要求）；
3. 训练环境声景（4 类）与人声异常（4 类）两个 MLP；
4. 同时评估 float 与 int8 量化后的指标，量化损失一目了然；
5. 导出 `vg_model_weights.c` 并写出 `reports/metrics.md`。

### 4. 验收线（PRD-07）

- 宏平均 F1 ≥ 0.80
- 每个安全类别召回率 ≥ 0.80
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
  → 统计聚合：均值(13) + 标准差(13) + 一阶差分均值(13) + 过零率(1) = 40 维
  → 两层 MLP（int8 权重 + float 偏置 + ReLU + Softmax）
      环境声景：40 → 24 → 4   (other / alarm_beep / water_flow / impact)
      人声异常：40 → 16 → 4   (none / moan / scream / shout_help)
```

| 预算项 | PRD-07 上限 | 当前 |
|---|---|---|
| 两个模型权重合计 | ≤ 1MB | **2.6KB** |
| 关键词/个性化模板 | ≤ 200KB | 16 条 × 176B ≈ **2.8KB** |
| 闪存总增量 | ≤ 2MB | 权重 + 特征代码 ≈ **60KB** |
| 单窗口处理耗时 | < 500ms | 主机侧 ~1ms，实机待标定（`velaguard status` 输出） |

## 为什么是 MLP 而不是 DS-CNN

PRD-07 的降级决断表写明：「板端量化推理示例接入 2 个工作日内未跑通 →
降级为 MFCC + 小型 MLP/线性分类器，权重导出 C 数组，不引入新推理框架」。

当前 openvela 工程内没有可直接在 Gemini-S1 上编译运行的量化推理示例，
且开发板未到手，因此**直接落地降级路线**：纯 C 前向、零第三方依赖、
体积与耗时都远低于预算，同时保留了升级到 DS-CNN 的空间——
只要替换 `vg_classifier.c` 的前向实现，特征与接口都不需要改。
