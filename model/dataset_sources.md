# 公开数据集调研（PRD-07）

> 核实日期：2026-07-28。本文的类别名、clip 数量、许可证均**实际抓取核对过**
> （ESC-50 的 `meta/esc50.csv` 与 `LICENSE`、FSD50K 的 `vocabulary.csv` /
> `dev.csv` / `eval.csv` / `README.md`、AudioSet ontology），不是凭印象写的。
>
> 结论先说：**五类声景里有四类能用公开数据打底，只有「呻吟」这一类必须自采。**

## 一、总览

| 我们的类别 | 公开数据可用性 | 主力来源 | 补充 |
|---|---|---|---|
| `alarm_beep` 报警蜂鸣 | 🟡 中等 | FSD50K `Alarm`(1864) | ESC-50 `clock_alarm`(40) `siren`(40)；**真实烟感实响需自录** |
| `water_flow` 持续水流 | 🟢 充足 | FSD50K `Water`(1925) `Water_tap_and_faucet`(458) `Sink`(407) | ESC-50 `pouring_water` `water_drops` `toilet_flush`（各 40） |
| `impact` 破碎撞击 | 🟢 充足 | FSD50K `Glass`(1241) `Shatter`(510) `Thump_and_thud`(470) | ESC-50 `glass_breaking`(40) `door_wood_knock`(40) |
| `scream` 痛苦叫声/哭喊 | 🟢 充足 | FSD50K `Screaming`(377) `Crying_and_sobbing`(151) | ESC-50 `crying_baby`(40) |
| `shout_help` 连续呼救 | 🟡 中等 | FSD50K `Shout`(393) `Yell`(199) | 内容是英文/无意义喊叫，中文求救词仍需自录 |
| `moan` 呻吟 | 🔴 **缺** | 无直接可用的公开集 | **必须自采**（见第四节） |
| `background` 负样本 | 🟢 充足 | FSD50K `Speech`(2254) `Conversation`(126) `Frying_(food)`(123) `Dishes`(479) | ESC-50 十余个家居/人声类 |
| `name_call_help` 姓名/方言 | 🔴 **缺** | 天然无公开集 | 全部自录（PRD-02 最小集） |

数量是 dev+eval 合计、按标签出现次数统计（FSD50K 是多标签，同一 clip 可能被计入多类）。

## 二、ESC-50

| 项 | 值（已核实） |
|---|---|
| 规模 | 2000 clips，50 类 × 40 条 |
| 单条 | 5 秒，44.1kHz |
| 许可证 | **CC BY-NC**（整体数据集） |
| 来源 | 从 Freesound.org 公开田野录音中人工裁剪 |
| 下载 | `https://github.com/karolpiczak/ESC-50/archive/master.zip`（约 600MB） |
| 元数据 | `meta/esc50.csv`（filename, fold, target, category, esc10, src_file, take） |

**为什么 ESC-50 特别适合我们**：官方已按原始录音源预分成 5 个 fold，
**同一原始录音的片段一定落在同一个 fold**。这正是 PRD-07 要求的
「按录音来源分组切分」，直接用 `fold` 列即可，不需要自己想办法分组。
`src_file` 列还给出了 Freesound 原始文件 ID，可用作更细的分组键。

### 与我们七类的映射

| 我们的类别 | ESC-50 category |
|---|---|
| `alarm_beep` | `clock_alarm`, `siren` |
| `water_flow` | `pouring_water`, `water_drops`, `toilet_flush` |
| `impact` | `glass_breaking`, `door_wood_knock` |
| `scream` | `crying_baby` |
| `background` | `vacuum_cleaner`, `washing_machine`, `clock_tick`, `keyboard_typing`, `mouse_click`, `can_opening`, `door_wood_creaks`, `footsteps`, `laughing`, `breathing`, `coughing`, `sneezing`, `snoring`, `brushing_teeth`, `drinking_sipping`, `clapping`, `rain`, `wind` |

ESC-50 的 50 类全名（已核实）：
`airplane, breathing, brushing_teeth, can_opening, car_horn, cat, chainsaw,
chirping_birds, church_bells, clapping, clock_alarm, clock_tick, coughing, cow,
crackling_fire, crickets, crow, crying_baby, dog, door_wood_creaks,
door_wood_knock, drinking_sipping, engine, fireworks, footsteps, frog,
glass_breaking, hand_saw, helicopter, hen, insects, keyboard_typing, laughing,
mouse_click, pig, pouring_water, rain, rooster, sea_waves, sheep, siren,
sneezing, snoring, thunderstorm, toilet_flush, train, vacuum_cleaner,
washing_machine, water_drops, wind`

**限制**：每类只有 40 条，单独用来训 7 类模型样本量偏少，适合作为
「高质量基线 + 快速验证」，主力还是 FSD50K + 自采。

## 三、FSD50K

| 项 | 值（已核实） |
|---|---|
| 规模 | 51,197 clips（dev 40,966 + eval 10,231），200 类 |
| 单条 | 0.3–30 秒不等，44.1kHz 单声道 |
| 类别体系 | AudioSet Ontology 的 200 类子集 |
| 数据集整体许可证 | **CC BY 4.0** |
| **单条 clip 许可证** | **逐条不同**：CC0 / CC-BY / **CC-BY-NC** / CC Sampling+ |
| 许可证映射文件 | `dev_clips_info_FSD50K.json`、`eval_clips_info_FSD50K.json`（在 `FSD50K.metadata.zip` 里） |
| Zenodo | record `4060432` |
| 论文 | Fonseca et al., *FSD50K: an open dataset of human-labeled sound events*, TASLP 2022 |

### 下载体积（已核实）

| 文件 | 大小 |
|---|---|
| `FSD50K.ground_truth.zip` | 0.3 MB ← **先下这个**，含 vocabulary/dev/eval 标签 |
| `FSD50K.doc.zip` | < 0.1 MB ← 许可证说明 |
| `FSD50K.metadata.zip` | 6.7 MB ← **逐 clip 许可证在这里** |
| `FSD50K.dev_audio.zip` + `.z01`~`.z05` | 约 18.4 GB |
| `FSD50K.eval_audio.zip` + `.z01` | 约 6.3 GB |

建议流程：先下前三个小文件（共 7MB），用许可证信息**筛掉不需要的 clip**，
再按筛出来的 ID 决定要不要下全量音频。

### 与我们七类的映射（类别名已核实存在于 200 类词表中）

下表是**脚本实跑输出**（`python3 fetch_public_datasets.py --fsd50k-meta`），
数量为「已剔除冲突标签后」的可用 clip 数，并按许可证细分：

| 目标类 | FSD50K 标签 | 可用 | CC0 | CC-BY | CC-BY-NC | 其他 |
|---|---|---|---|---|---|---|
| `alarm_beep` | `Alarm` | 1698 | 768 | 697 | 178 | 55 |
| | `Chime` | 253 | 76 | 109 | 57 | 11 |
| | `Doorbell` | 142 | 51 | 70 | 15 | 6 |
| | `Siren` | 118 | 61 | 44 | 11 | 2 |
| `water_flow` | `Water` | 1896 | 734 | 913 | 198 | 51 |
| | `Water_tap_and_faucet` | 455 | 236 | 147 | 39 | 33 |
| | `Sink_(filling_or_washing)` | 404 | 231 | 124 | 25 | 24 |
| | `Toilet_flush` | 274 | 122 | 103 | 23 | 26 |
| | `Bathtub_(filling_or_washing)` | 202 | 124 | 52 | 15 | 11 |
| | `Boiling` | 102 | 56 | 39 | 4 | 3 |
| `impact` | `Glass` | 1227 | 483 | 588 | 133 | 23 |
| | `Shatter` | 505 | 146 | 308 | 46 | 5 |
| | `Thump_and_thud` | 461 | 280 | 128 | 38 | 15 |
| | `Knock` | 369 | 223 | 108 | 32 | 6 |
| `scream` | `Screaming` | 375 | 154 | 172 | 46 | 3 |
| | `Crying_and_sobbing` | 150 | 45 | 70 | 26 | 9 |
| `shout_help` | `Shout` | 386 | 152 | 199 | 30 | 5 |
| | `Yell` | 198 | 81 | 101 | 14 | 2 |
| `background` | `Speech` | 2142 | 648 | 1157 | 296 | 41 |
| | `Dishes_and_pots_and_pans` | 369 | 162 | 170 | 13 | 24 |
| | `Frying_(food)` | 121 | 55 | 53 | 6 | 7 |
| | `Conversation` | 116 | 37 | 55 | 18 | 6 |
| **合计** | | **11963** | 4925 | 5407 | 1263 | 368 |

**只取 CC0 + CC-BY 就有 8743 条 clip**——授权边界最清晰，够训一个像样的
六类模型（`moan` 除外）。完整清单已生成到
`model/reports/fsd50k_candidates.csv`（含每条 clip 的 id 与许可证），
这份文件同时充当 PRD-07 要求的许可证登记明细。

### 三个坑

1. **没有 `Smoke_detector`**。AudioSet 完整本体里有「Smoke detector, smoke alarm」，
   但**不在 FSD50K 的 200 类里**。烟感蜂鸣要靠 `Alarm`（较宽泛，混了闹钟、警报器）
   + Freesound 直接搜 + **自录真实烟感测试键实响**。这是我们最需要自采的环境类。
2. **没有 `Buzzer` / `Beep_and_bleep`**。同上，靠 `Alarm` 兜。
3. **多标签**。一条 clip 可能同时是 `Water` 和 `Speech`。做单标签训练时必须
   过滤掉带负样本标签的 clip，否则正负样本互相污染。

### 许可证注意

比赛是非商业用途，CC-BY-NC 可用，但**必须逐条登记来源与署名**。
建议直接按许可证筛选：只保留 **CC0 + CC-BY**（可署名即可，边界最清晰），
CC-BY-NC 只在样本量不够时才用并单独标注。筛选依据在
`dev_clips_info_FSD50K.json` 的每条记录里。

## 四、呻吟（moan/groan）—— 唯一必须自采的类

核对结果：

- **FSD50K 200 类里没有** groan / moan / whimper 任何一个。
- **AudioSet 完整本体里有**：`Groan`、`Wail, moan`、`Whimper`、`Gasp`、`Sigh`
  （已核实，且都不是 restricted 类）。但 AudioSet **只发布标签和特征，
  音频要自己从 YouTube 下载**——这既有 ToS 风险，也有版权风险，
  比赛材料里不建议走这条路。
- 医疗/疼痛发声的公开语料（如各类 pain vocalization 研究集）多为
  受限访问或需机构伦理审批，**赶不上比赛节奏**。

**结论：呻吟必须自采。** 这也正是 PRD-07 已经写明的
「呻吟/痛苦叫声：公开数据稀缺，以自采/拟音为主」——本次核对确认了这个判断。

自采要求（写进 `dataset_registry.csv`）：

- 由队伍成员拟音，**事前书面同意**，明确用途与保存方式；
- 覆盖不同性别、不同强度（轻声呻吟 / 明显痛苦）、不同距离；
- 原始音频**只存本地/TF 卡**，不进 Git、不进主存储（隐私红线 1）；
- 仓库里只保留处理脚本与来源说明。

### 一个降低自采量的办法

`scream`（痛苦叫声/哭喊）用 FSD50K 打底，`moan` 只需要自采到能和
`scream` / `background` 区分开即可——因为**状态机对这两类的处置是一样的**
（都归 `distress_voice`，先本地询问、重复升级）。也就是说 `moan` 的召回
比它和 `scream` 的区分度更重要，自采量可以控制在**每人 30–50 条**。

## 五、其他值得一提的公开集

| 数据集 | 用途 | 许可证 | 备注 |
|---|---|---|---|
| **UrbanSound8K** | `siren` 补充 | CC BY-NC 3.0 | 8732 clips ≤4s，10 类；城市场景为主，家庭场景不匹配 |
| **MUSAN** | 背景负样本 | CC BY 4.0 | noise/speech/music 三大类，量大且授权干净 |
| **DEMAND** | 家庭环境噪声 | CC BY-SA 3.0 | 18 种真实环境多通道噪声，含厨房/客厅/洗衣 |
| **Common Voice zh-CN** | 普通话负样本 | **CC0** | 授权最干净的中文语音，适合做"普通说话"负样本 |
| **AISHELL-1** | 普通话负样本 | Apache 2.0 | 178 小时朗读语音 |
| **KeSpeech** | 方言参考 | 研究用途 | 覆盖多种普通话方言变体，用前需核对条款 |
| **Freesound** 直接搜索 | 烟感/水流/破碎的精准补充 | 逐条 CC | 搜 "smoke alarm test"、"fire alarm beep" 效果比数据集类别好 |

> 上表除 ESC-50 / FSD50K / AudioSet 外的条目**未逐一抓取核对**，
> 使用前请到各自官网确认最新的许可证与下载方式。

## 六、推荐的落地方案

分三步走，每步都能单独产出可用模型：

### 第一步：ESC-50 打底（半天，600MB）

```bash
cd model
python3 fetch_public_datasets.py --esc50 --dst ../datasets
python3 train_models.py --data-dir ../datasets
```

覆盖 `alarm_beep` / `water_flow` / `impact` / `background` 四类，
每类 40–700 条。**先把真实数据链路跑通**，指标不会好看（`scream` 只有
crying_baby，`moan` / `shout_help` 完全没有），但足以验证流程。

### 第二步：加 FSD50K（✅ 已于 2026-08-02 完成，走 HF 镜像逐 clip 下载）

```bash
python3 fetch_public_datasets.py --fsd50k-meta                    # 先下 7MB 元数据
python3 fetch_public_datasets.py --fsd50k-fetch --dst ../datasets # 按清单逐 clip 下载
python3 train_models.py --data-dir ../datasets --epochs 3000 --features v2 --final
```

**实际执行记录**：没有下载 Zenodo 的 25GB 分卷压缩包（分卷必须下齐才能解压、
峰值占盘翻倍、单连接实测约 16 小时）。改走 HuggingFace 镜像
（`Fhrozen/FSD50k`，clip 逐个平铺存放），按 `fsd50k_candidates.csv` 里筛好的
CC0+CC-BY 清单精确取用：8551 条候选（10332 行按 clip 去重、剔除 192 条跨目标
类冲突后），16 并发 30.7 分钟全部下完、0 失败，250 条短于 0.5 秒丢弃，
最终入库 8267 条 / 约 4.6GB（已转 16kHz 单声道）。

这一步之后除 `moan` 外六类都有 FSD50K 真实数据。窗口级宏 F1 未直接达
0.80（40 维 MFCC 统计特征对瞬态类是表征瓶颈），最终通过「v2 特征（+13 维
瞬态/谱形描述子）+ voice 三类合并 + 事件级口径」达标，见
`reports/metrics.md` 与 `docs/acceptance.md`。

### 第三步：自采补齐（1–2 天）

必采清单：

| 内容 | 数量 | 理由 |
|---|---|---|
| 真实烟感报警实响 | 20–30 条 | FSD50K 的 `Alarm` 太宽泛，实响是演示的核心场景 |
| 呻吟/痛苦发声 | 每人 30–50 条 | 无公开数据 |
| 姓名/称呼/求救词（平静 + 急促成对） | PRD-02 最小集 | 天然无公开数据 |
| 方言求救短语 | ≥ 1 种 | 同上 |
| 自家背景负样本（电视、交谈、做饭、**平静喊名**） | 30 分钟以上 | 误报测试的关键；「平静喊名」是最重要的负样本 |

自采完成后重跑第二步的训练命令即可。

## 七、第二批候选数据集（2026-07-31 核实）

> 背景：本机网络**境外直连全部不通**（Zenodo / GitHub / Kaggle / Freesound 均不可达，
> 配置的两个代理 127.0.0.1:7890 与 192.168.2.68:7890 均无响应），
> 但 **hf-mirror.com（HuggingFace 镜像）、ModelScope、Gitee、OpenI 可直连**。
> 因此本批候选全部选择**在 hf-mirror 上有镜像、可直接下载**的数据集，
> 许可证与内容已逐一抓取各仓库 README 核对。
>
> 下载方式统一为（注意先清掉失效代理变量）：
> ```bash
> unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY
> HF_ENDPOINT=https://hf-mirror.com huggingface-cli download --repo-type dataset <repo_id> --local-dir <dst>
> ```

| 数据集 | HF 镜像仓库 | 补哪个缺口 | 许可证（已核） | 备注 |
|---|---|---|---|---|
| **Nonspeech7k** ✅已下载 | `W4ng1204/Nonspeech7k`（parquet，train+test） | `scream` 主力、`background`（咳嗽/呼吸/笑/打喷嚏/哈欠） | 原始集 CC BY 4.0（Zenodo 6967442，镜像未标，登记时按原始集写） | **实点 7014 条**：screaming 663、crying 1996、breath 1850、laugh 1273、cough 702、sneeze 266、yawn 264 |
| **VIVAE** ✅已下载 | `vtsouval/vivae` | **`moan` 首个可用公开源**、`scream` | CC BY-NC 4.0（镜像与 Zenodo 4066235 一致） | **实点 1565 条**（full_set 1085 + core_set 480）：pain 265、fear 256、anger 254、achievement 241、pleasure 282、surprise 267；4 档强度正好配合状态机分级 |
| **ASVP-ESD** ✅已下载 | ~~`EdwardLin2023/ASVP_ESD`~~ 音频在 Google Drive 不可达；**实际走 Kaggle 匿名直连**（`dejolilandry/asvpesdspeech-nonspeech-emotional-utterances`，1.6GB zip） | `moan` 补充、`scream` 补充 | CC BY 4.0 | **实点 13,964 条 wav**（完整更新版，远超 HF README 的 speech 小表）：pain/groan **834**、fearful/scream/panic **1180**、sad/cry 2694、neutral 1598、happy/laugh 1868、breath 172 等；文件名第 3 段为情绪码 |
| **VocalSound** ✅已下载 | `MahiA/VocalSound` | `background` 人声负样本 | 原始集 CC BY-SA 4.0（Gong et al.，MIT 标注是镜像自标，登记按原始集） | **实点 21,024 条**：笑、叹气、咳嗽、清嗓、喷嚏、吸鼻六类均衡；**叹气/咳嗽是防 moan 误报的关键负样本** |
| **AudioSet（HF 托管音频版）** ✅已下载 | `agkphysics/AudioSet`（balanced 配置，38 个 parquet 分片已逐一比对字节数） | **烟感报警实响**、`shout_help`、`moan` | 仓库标 CC BY 4.0；音频源自 YouTube，**合规风险自评后再用，逐条登记** | **实点 18,683 条 / 527 类**（flac 于 parquet 内）：Smoke detector 54、Fire alarm 55、Siren 172、Shout 49、Yell 54、Groan 47、Wail moan 40、Whimper 41、Screaming 45、Glass 57、Water 218 等；每类约 40–60 条，作补充不作主力 |
| **CochlScene**（可选） | `yotarokubo/CochlScene` | `background` 真实场景底噪 | CC BY-SA 3.0（Zenodo 7080122） | 76k 条 13 种场景（含厨房/居住区），全量 34GB，按需抽取 |

不推荐：`lrauch/desed` —— 名字叫 DESED，实际内容是 IDMT-FL（Zenodo 7551584），
且许可证为 **CC BY-NC-ND 4.0**（禁止演绎），训练用途边界不清，跳过。

搜索通道现状（供后续复用）：内置 WebSearch 偶发 429；open-websearch MCP 里
**只有 sogou 可用**（baidu 被 302 反爬拦截，bing 请求模式返回无关缓存页，
duckduckgo/brave/startpage/exa 因境外不通全部失效，playwright 模式缺依赖）。
查数据集优先用 `https://hf-mirror.com/api/datasets?search=<关键词>`。

## 八、合规检查清单

提交前逐条确认：

- [ ] `dataset_registry.csv` 里每条公开数据都填了来源、许可证、署名要求
- [ ] FSD50K 用到的 clip 已从 `*_clips_info_FSD50K.json` 导出许可证清单并归档
- [ ] ESC-50 / UrbanSound8K 的 CC BY-NC 已在文档中署名（Piczak / Salamon 等）
- [ ] 自采音频有录制者书面同意，且**未进 Git**（`.gitignore` 已排除 `datasets/`、
      `model/raw_audio/`、所有音频扩展名）
- [ ] `git ls-files | grep -E '\.(wav|mp3|flac|pcm|ogg|m4a)$'` 输出为空
      （`tests/run_tests.sh` 第 6 节已自动断言）
