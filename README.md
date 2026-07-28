# 安聆 VelaGuard —— 无摄像头家庭异常声景 AI Agent

> 2026 首届 openvela AI 硬件开发者大赛参赛作品 ｜ 队伍编号 121 ｜ 队伍名 jieyoukeneng

## 一、作品简介

**一句话：** 面向独居长辈家庭看护，设备在本地听懂异常声音和求助人声，
先在家里提醒，没人回应才升级为家属手机上的远程通知——**全程没有摄像头，
原始录音不出设备**。

老人独居出事，往往不是"没人管"，而是"没人知道"。装摄像头能解决知情问题，
但很多老人不接受被拍。安聆换了条路：**只听、不看，而且听到的东西不出门。**

设备持续采集音频，在端侧识别五类安全声景，用可解释的规则状态机把它们
转成三级事件：

| 声景 | eventType | 规则（演示档） |
|---|---|---|
| 烟雾/燃气报警蜂鸣 | `alarm_beep` | 高置信度连续 5 秒 → **直接紧急级** |
| 呻吟/痛苦叫声/哭喊/连续呼救 | `distress_voice` | 先本地询问，重复或无响应 → 升级 |
| 持续水流/疑似漏水 | `water_flow` | 30s 提醒 → 120s 警告 → 超时紧急 |
| 姓名/亲属称呼/方言求救词 | `name_call_help` | **平静单次只本地确认，急促重复才升级** |
| 玻璃破碎/重物撞击 | `impact` | 单次警告；夜间或 60s 内重复 → 紧急 |

### 三个我们认为最值得看的点

**1. 「听到名字就报警」是这类产品最容易翻车的地方，我们从代码层面堵死了。**
平静地喊一声"老伴"只会在设备上亮个提醒，倒计时结束也只是关掉事件，
**永远不会打扰家属**（`local_only` 标记，见 `vg_rule_name_call`）。
只有急促且重复、或明确求救词、或伴随呻吟等异常人声，才会走升级路径。

**2. 端侧优先不是口号，是每一环都能单独失效。**
控制台挂了、Agent 挂了、LLM 挂了、网断了、`/data` 写不了、麦克风没接——
本地识别、提醒、确认、日志照常工作。通知文案有三级回退
（ai_agent → Skill 规则表 → 端侧内置模板），**不因 Agent 失败丢通知**。
这条在 `tests/run_tests.sh` 里是自动化断言，不是文档承诺。

**3. 隐私边界有三道代码级硬约束，可当场演示。**
事件结构体里没有音频/文本字段；Skill 指令里禁止索取音频；
控制台有字段黑名单，带 `transcript` 的负载直接 400 拒绝。

## 二、选题方向

**AI 硬件产品创新**。

目标硬件是全志润芯微 **Gemini-S1 R528**（openvela / NuttX，`dev-ai-contest-2026`）。
开发板尚未到手，因此全程**双线并行**：

- **模拟器线（已完成）**：事件协议、风险状态机、三页面 UI、识别引擎、
  个性化录入、Skill 与通知控制台，全部在 openvela 模拟器
  （`goldfish-arm64-v8a-ap`）+ 电脑端跑通闭环，并且是演示兜底方案；
- **实机线（待板到手）**：板载麦克风采集、按键/屏幕/扬声器/LED 真实驱动、
  录放并发标定、真实数据集模型指标。

哪些已验证、哪些待实机，`docs/acceptance.md` 逐条列了，没有含糊其辞。

## 三、目录结构

```text
contest2026_121_jieyoukeneng/
├── app/velaguard_device/        # 端侧应用（linkfile → packages/demos/contest2026_121_velaguard）
│   ├── common/                  #   事件协议、极简 JSON、时钟与夜间判定、运行配置
│   ├── audio_detector/          #   音频源、MFCC 特征、int8 MLP 推理、窗口调度、模型权重
│   ├── enrollment/              #   个性化模板录入 / 管理 / 匹配
│   ├── event_state_machine/     #   三级状态机、五类规则、并发仲裁、snooze
│   ├── hardware_adapter/        #   LCD 三页面（文本 + LVGL）、RGB LED、提示音、按键
│   ├── event_log/               #   100 条环形事件日志，写入即落盘
│   ├── uploader/                #   HTTP、notifier 三级回退、Skill 桥接、幂等重试队列
│   ├── velaguard_daemon.c       #   守护主循环
│   └── velaguard_main.c         #   NSH 命令入口 + 内置 24 项验收自检
├── agent_skill/anling-home-safety/  # 自定义 ai_agent Skill（部署到 /data/agent/skills/）
├── model/                       # 数据登记表、特征、合成自举数据集、训练与 int8 量化导出
├── prototype/console/           # 通知与演示控制台（Node 零依赖 + SQLite + SSE）
├── prototype/simulate/          # 五类模拟事件注入脚本（板前联调 + 演示兜底）
├── tests/                       # 主机侧回归（18 项断言）+ 特征一致性校验 + 性能标定
├── docs/                        # 架构说明、验收对照与实测结果、演示脚本
├── logs/                        # AI Coding 日志
├── config.example.json          # 配置模板（真实配置含凭证，不进 Git）
└── board/ quickapp/ app/hello_app/   # 组委会示例骨架，保留备查
```

## 四、运行方式

### 方式 A：主机侧最快复现（30 秒，无需 openvela 工程）

端侧核心模块只依赖 C 标准库与 POSIX，可直接在开发机编译：

```bash
cd tests
make && make test
```

预期输出结尾：

```
======================================
  回归结果：通过 18 项，失败 0 项
======================================
```

### 方式 B：openvela 模拟器（完整闭环，推荐评委走这条）

```bash
# 1) 拉取工程（在工作区根目录）
repo init -u https://github.com/open-vela/contest2026_121_jieyoukeneng \
  -b dev-ai-contest-2026 -m contest2026_121_jieyoukeneng.xml
repo sync -c -j8

# 2) 启用本作品（这一步改的是工作区文件，不属于本仓提交内容）
echo "CONFIG_LVX_USE_DEMO_CONTEST2026_121_VELAGUARD=y" \
  >> vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/defconfig

# 3) 编译（约 2 分钟，全新环境首次约 8 分钟）
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j$(nproc)

# 4) 运行模拟器
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap
#   有图形界面时去掉 -no-window；无显示环境加 -no-window
```

看到 `goldfish-armv8a-ap>` 提示符后：

```sh
velaguard selftest                 # 24 项内置验收自检，约 20 秒
velaguard bench 60                 # 识别链路性能标定
velaguard sim alarm_beep --conf 0.95 --times 6 --interval 1000
velaguard ui event                 # 事件确认页（与 LCD 同款内容）
velaguard ack handled
velaguard log 5                    # 本地事件日志
velaguard status                   # 运行状态、性能、硬件可用性
```

> NSH 单行命令长度有限，参数多时建议分几条执行。

### 方式 C：家属侧远程通知闭环

```bash
# 终端 1：起控制台（零 npm 依赖，Node >= 22.5）
cd prototype/console && node server.js

# 手机/浏览器打开 http://<笔记本局域网IP>:8080/   ← 家属视角

# 终端 2：注入事件（板前联调或演示兜底）
cd prototype/simulate && node inject_events.js

# 或者从设备上传（先把 consoleHost 写进 /data/velaguard/config.json）
velaguard notify test
```

### 方式 D：实机（Gemini-S1，板到手后）

```bash
cp packages/ai_agent/defconfigs/gemini-s1/gemini-s1_defconfig \
  vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/defconfig
echo "CONFIG_LVX_USE_DEMO_CONTEST2026_121_VELAGUARD=y" \
  >> vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/defconfig
bash packages/ai_agent/fix_gemini_s1.sh
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/ \
  -e -Wno-error -j"$(nproc)"
```

烧录后：

```sh
velaguard start --mic              # 启动守护，接板载麦克风
velaguard duplex on                # 若实测支持录放并发
```

### 配置

真实配置放 `/data/velaguard/config.json`（含 Wi-Fi、控制台地址、可选 webhook，
**不进 Git**）。模板见 `config.example.json`，字段含义都有中文注释。

### 部署自定义 Skill

```bash
mkdir -p /data/agent/skills/anling-home-safety
# 拷入 agent_skill/anling-home-safety/{SKILL.md,advice.json}
```

部署后 `velaguard status` 的「通知文案」会显示来源为 `Skill 规则表`；
不部署也能正常通知（回落到端侧内置模板）。

## 五、Agent 能力与「主动 + 执行」场景

`anling-home-safety` 是本作品的自定义 ai_agent Skill：

- **输入**：只有结构化事件（类型/时间/持续/置信度/是否响应/夜间/时间可信度），
  **绝不接收原始音频或对话文本**；
- **输出**：一句 ≤ 60 字、家属能立刻据以行动的中文通知文案 + 建议动作；
- **执行**：通过 `velaguard notify --json '<事件JSON, 含 advice>'` 把文案回灌给
  端侧 notifier，由 notifier 负责发送与断网补发。

**「主动」体现在**：不是用户问了才答，而是设备在无人交互时自己判断风险等级、
自己决定要不要打扰人、自己决定什么时候升级找家属。
**「执行」体现在**：升级动作是端侧闭环执行的——播提示音、亮红灯、唤醒屏幕、
POST 摘要、断网入队、恢复补发，全部不需要人介入。

## 六、AI Coding 使用说明

本作品从需求拆解到编码、调试、文档全流程使用 Claude Code 协作，
完整对话日志见 `logs/`。几个 AI 实际起了关键作用的点：

**需求拆解**：先把总计划拆成 `docs/prd/` 的 00–08 九份子 PRD，明确每个模块的
接口、验收标准与降级路线，再逐个模块实现。这让「哪些属于模拟器线、
哪些必须等开发板」从一开始就清晰，不会做到一半才发现被硬件卡住。

**接口先行**：先定 `vg_types.h` 里的 `SoundObservation` / `DistressObservation` /
`SafetyEvent` 三个结构体和状态机回调，再并行铺开各模块。识别层只输出
类别与置信度、决策全部在状态机——这个边界是设计时定死的，后面没有反复。

**自动化验证代替人工点检**：`velaguard selftest` 的 24 项断言和
`tests/run_tests.sh` 的 18 项回归都是在写功能的同时一起写的。
其中**端侧 C 特征与训练脚本 Python 特征逐维一致性校验**这一条是 AI 提醒后
补上的，实测最大误差 3.0e-05——没有这一条，电脑端的训练指标在板上是不可信的。

**调试**：模拟器上跑出过一个 `lib_fdcheck.c` 断言 panic。定位过程是
AI 读 backtrace → 指出 NuttX 里「文件描述符属于任务、静态变量却跨任务」→
改成按需 open/close。修完又发现新问题：为修 fd 而做的「换任务就重新初始化」
把状态机轨道也清空了，导致 `velaguard sim` 之后 `velaguard ack` 找不到事件。
最终区分了**进程私有资源**与**跨命令共享状态**（`vg_sm_set_callbacks`）。
这两个坑写进了 `docs/architecture.md`，因为它们是 NuttX 上真实会踩的。

**效率**：约一个工作日内完成了 ~7000 行 C + Python + Node 代码、
一套自动化回归、一份可运行的模型训练流水线和全部交付文档。
AI 在「样板代码 + 交叉一致性检查 + 文档同步」上贡献最大；
产品判断（比如"平静喊名字绝不能打扰家属"该怎么落到代码）仍然由人来定。

## 七、已知限制

诚实列出，不回避：

1. **模型指标目前来自自举合成数据集**，F1 接近 1.0 是因为合成信号人为可分，
   **不能作为 PRD-07 验收依据**。它证明的是「特征 → 训练 → int8 量化 →
   端侧 C 推理」链路已打通。真实数据集流程见 `model/README.md`。
   （有一项是真实有效的：训练集外随机种子生成的 7 类保留样本，
   端侧 C 推理逐窗口分类全部正确。）
2. **未在真实硬件上验证**：麦克风采集、录放并发、WS2812、LRADC 键位、
   实机性能与内存，全部待开发板到手。代码路径已实现并做了降级，
   但没有实机数据就不写实机结论。
3. **LCD 目前显示 ASCII 文案**：LVGL 默认字体不含 CJK 字形。控制台与远程
   通知是完整中文。补一份 CJK 子集字体后，把 `vg_ui_render(..., ascii=false)`
   即可切换。
4. **端侧模型是 MLP 不是 DS-CNN**：这是 PRD-07 降级决断表的既定路线
   （工程内无可在 Gemini-S1 跑通的量化推理示例）。接口不变，可平滑替换。
5. **方言只覆盖用户主动录入的短语**，不做完整连续语音转写，不承诺覆盖所有方言。
6. **控制台不是云服务**：只在局域网内工作，也不打算做成公网服务。

## 八、测试环境

| 项 | 版本 |
|---|---|
| 操作系统 | Ubuntu 22.04 (Linux 6.8) |
| openvela | `dev-ai-contest-2026` |
| 编译目标 | `vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap` |
| 主机编译器 | gcc 11 (C11) |
| Python | 3.10 + numpy 2.2 |
| Node.js | 22.23（用到内置 `node:sqlite`，需 ≥ 22.5） |

实测数据与复现命令见 [`docs/acceptance.md`](docs/acceptance.md)。

## 九、延伸阅读

| 文档 | 内容 |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | 数据流、源码地图、状态机、识别引擎、可靠性设计、两个 NuttX 坑 |
| [`docs/acceptance.md`](docs/acceptance.md) | PRD-08 十项验收对照、实测性能与资源、待实机项、与 PRD 的偏差说明 |
| [`docs/demo.md`](docs/demo.md) | ≤ 5 分钟演示脚本与兜底方案 |
| [`model/README.md`](model/README.md) | 数据来源与许可证、训练与量化流程、模型预算 |
| [`agent_skill/README.md`](agent_skill/README.md) | Skill 部署、三级文案回退、隐私边界 |
| [`prototype/console/README.md`](prototype/console/README.md) | 控制台接口、家属视角页面、协议与隐私校验 |
| [`tests/README.md`](tests/README.md) | 主机侧回归覆盖内容与用法 |
