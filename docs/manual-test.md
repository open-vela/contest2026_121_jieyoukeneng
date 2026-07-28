# 安聆 VelaGuard 手动测试手册

> 两部分：**A. 模拟器手动测试**（不依赖开发板，任何时候都能做）、
> **B. 真机手动测试**（Gemini-S1 到手后做）。
>
> 每个用例给出：操作 → **预期结果** → 判定标准。预期结果里带具体数字的，
> 都是本仓已实测过的值，可直接对照。
>
> 自动化部分见 `tests/run_tests.sh`（18 项）与 `velaguard selftest`（24 项）；
> 本手册覆盖的是**自动化测不了或需要人眼确认**的部分。

---

# A. 模拟器手动测试

## A0. 环境准备

| 项 | 要求 |
|---|---|
| 操作系统 | Ubuntu 22.04（官方兼容环境；WSL/Docker 不作最终验证） |
| Node.js | ≥ 22.5（控制台用到内置 `node:sqlite`） |
| Python | 3.10 + numpy |
| openvela | 已 `repo sync` 完成的工作区 |

```bash
# 在工作区根目录（本仓的上一级）
echo "CONFIG_LVX_USE_DEMO_CONTEST2026_121_VELAGUARD=y" \
  >> vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/defconfig

./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j$(nproc)
```

**预期**：最后一行 `#### build completed successfully`，耗时全新环境约 8 分钟、
增量约 2 分钟。产物 `cmake_out/vela_goldfish-arm64-v8a-ap/nuttx`。

> 这一步改的是**工作区文件**（不属于本仓），符合 PRD-00「生产仓库零改动」——
> 我们没有动 `packages/`、`nuttx/`，只在 board defconfig 追加了一行开关。

启动模拟器：

```bash
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap
# 无图形环境时加 -no-window
```

**预期**：约 40 秒后出现 `goldfish-armv8a-ap>` 提示符。

> **NSH 单行长度有限**（约 80 字符），参数多的命令请拆成几条，
> 否则会被截断成两条命令并报 `command not found`。

---

## A1. 冒烟测试（2 分钟）

| # | 操作 | 预期结果 | 判定 |
|---|---|---|---|
| A1.1 | `velaguard` | 打印中文用法说明，含 start/sim/ack/ui/log/feed/enroll/selftest/bench | ☐ |
| A1.2 | `velaguard selftest` | 逐场景打印，结尾 `==== 自检结果: 通过 24 项，失败 0 项 ====` | ☐ |
| A1.3 | `velaguard status` | 打印运行状态；「模型」行显示 `MFCC(40)+MLP int8 ... 权重 2592 字节` | ☐ |
| A1.4 | `velaguard config` | 打印配置，**不出现任何密码/密钥/webhook 地址** | ☐ |

A1.2 是最关键的一条：24 项断言覆盖了 PRD-08 十项验收场景里可自动化的全部内容。
**任何一项失败都不要继续往下测**，先看失败项的说明。

---

## A2. 五类事件的分级与升级（PRD-03）

先清空历史，保证结果可对照：

```sh
rm -rf /data/velaguard
```

### A2.1 烟雾/燃气报警 → 直接紧急级

```sh
velaguard sim alarm_beep --conf 0.95 --times 6 --interval 1000
```

**预期**：
- 打印 `事件 evt_xxxxxx 烟雾/燃气报警/紧急 状态=emergency`
- 打印 `无人确认，升级为紧急级并触发远程通知`
- 页面显示 `[紧急] 烟雾/燃气报警`

**判定**：☐ 直接到紧急级（**不经过警告级**，这是 PRD-03 规定的"连续 5 秒直接紧急"）

### A2.2 持续水流 → 提醒 → 警告 → 超时紧急

```sh
velaguard ack handled
velaguard sim water_flow --conf 0.85 --times 12 --interval 1000
```

**预期**：先出现 `持续水流/提醒`（约第 10 秒虚拟时间），继续注入后升为 `警告`。

```sh
velaguard sim water_flow --conf 0.85 --times 20 --interval 1000
velaguard ui event
```

**判定**：
- ☐ 先提醒后警告，**不是一上来就警告**（短时水流不打扰人）
- ☐ 事件确认页显示倒计时秒数

### A2.3 破碎/撞击 → 警告要求确认

```sh
velaguard ack handled
velaguard sim impact --conf 0.9
velaguard ui event
```

**判定**：☐ 单次撞击 → 警告级（日间），页面提示三种确认操作

### A2.4 呻吟/呼救 → 先本地询问，重复升级

```sh
velaguard ack handled
velaguard sim distress_voice --conf 0.95 --urgency 0.9 --times 3 --interval 900
```

**判定**：
- ☐ 第一次命中是**警告级**（本地询问"是否需要帮助"）
- ☐ 重复后升为**紧急级**并触发远程通知

### A2.5 平静喊名字 → 只提醒、**永不远程通知**（本作品最关键的一条）

```sh
velaguard ack handled
velaguard sim name_call_help --phrase 老伴 --urgency 0.2
velaguard ui event
```

**预期页面**：

```
[提醒] 呼喊姓名/求救词
匹配短语: 老伴 (...) 重复 1 次
平静单次呼喊：仅本地确认，不远程通知
```

**判定**：
- ☐ 等级是**提醒**，不是警告或紧急
- ☐ 页面明确出现「仅本地确认，不远程通知」
- ☐ 控制台**没有**收到这条事件（见 A4）

### A2.6 急促重复呼救 → 升级

```sh
velaguard ack handled
velaguard sim name_call_help --phrase 救命 --urgency 0.9 --times 3
```

**判定**：☐ 升到紧急级并触发远程通知（与 A2.5 形成对照，这是演示的高光点）

---

## A3. 本地确认与并发仲裁（PRD-03/04）

| # | 操作 | 预期结果 | 判定 |
|---|---|---|---|
| A3.1 | 产生一个警告事件后 `velaguard ack handled` | 打印 `事件结束 ... -> 已处理`，事件关闭且不再升级 | ☐ |
| A3.2 | `velaguard ack false_alarm` | 打印 `-> 误报`，记录反馈但不升级 | ☐ |
| A3.3 | `velaguard ack snooze` ×2 | 两次都成功，状态在 `snoozed` / `warning` 间流转 | ☐ |
| A3.4 | 第 3 次 `velaguard ack snooze` | 返回「稍后提醒次数已用尽」 | ☐ |
| A3.5 | 并发：先注入水流到提醒级，再注入呼救 | `velaguard ui event` 顶部显示**呼救**，下方「排队事件」列出水流 | ☐ |
| A3.6 | A3.5 后 `velaguard ack handled` | 呼救关闭，页面自动切到水流事件 | ☐ |

并发用例的命令（注意 NSH 行长，分两条）：

```sh
velaguard sim water_flow --conf 0.85 --times 12 --interval 1000
velaguard sim distress_voice --conf 0.95 --urgency 0.8
velaguard ui event
```

---

## A4. 家属侧远程通知闭环（PRD-06）

模拟器与控制台在同一台机器上时，用回环地址即可。

**注意**：模拟器内的网络是 NAT 的，模拟器访问宿主机要用 `10.0.2.2`（goldfish 默认网关），
不是 `127.0.0.1`。

```bash
# 宿主机终端 1
cd prototype/console && node server.js
# 浏览器打开 http://127.0.0.1:8080/
```

```sh
# 模拟器 NSH：写配置指向宿主机
mkdir -p /data/velaguard
echo '{"consoleHost":"10.0.2.2","consolePort":8080}' > /data/velaguard/config.json
velaguard notify test
velaguard status
```

| # | 检查点 | 判定 |
|---|---|---|
| A4.1 | `velaguard status` 的「上传」行显示 `在线`、`成功 1` | ☐ |
| A4.2 | 浏览器页面**自动弹出**卡片（无需刷新），含类型/等级/时间/持续/置信度/建议动作 | ☐ |
| A4.3 | 卡片左侧色条与等级一致（提醒绿 / 警告黄 / 紧急红） | ☐ |
| A4.4 | 紧急卡片有提示音（需浏览器允许音频）与呼吸高亮 | ☐ |
| A4.5 | 点卡片上「已联系/已处理」，状态标签变为「已处理」 | ☐ |
| A4.6 | A2.5 的平静喊名事件**没有出现在页面上** | ☐ |

> 如果模拟器网络不通（A4.1 显示离线），**不影响其余用例**——
> 这正是设计要证明的"端侧不依赖网络"。此时可改用宿主机版
> `tests/build/velaguard` 做 A4，效果等价（见 `tests/README.md`）。

### A4.7 断网补发幂等

```bash
# 宿主机：Ctrl-C 关掉控制台
```

```sh
velaguard sim alarm_beep --conf 0.95 --times 6 --interval 1000
velaguard status          # 「上传」行应显示 离线，待发 >= 1
```

```bash
# 宿主机：重新 node server.js
```

```sh
velaguard flush
velaguard status          # 待发应回到 0
```

| # | 检查点 | 判定 |
|---|---|---|
| A4.7a | 断网期间本地提醒**照常**（有事件打印、页面有内容） | ☐ |
| A4.8 | 恢复后卡片补上，且**同一 eventId 只有一张卡片**（不重复） | ☐ |

---

## A5. Agent Skill 与文案回退（PRD-06）

```sh
# 未部署 Skill 时
velaguard status          # 「通知文案」行应显示 来源 内置模板
```

部署 Skill（模拟器可用 adb push，或把文件放进 `/data` 分区镜像）：

```sh
mkdir -p /data/agent/skills/anling-home-safety
# 拷入 agent_skill/anling-home-safety/{SKILL.md,advice.json}
velaguard status          # 「通知文案」行应显示 来源 Skill 规则表
```

| # | 检查点 | 判定 |
|---|---|---|
| A5.1 | 未部署 Skill 时通知**照常发出**（用内置模板文案） | ☐ |
| A5.2 | 部署后 status 显示 `Skill 规则表`，卡片建议文案更贴合事件类型 | ☐ |
| A5.3 | `velaguard notify --json '{...,"advice":"自定义文案"}'` 后，卡片上显示的正是这段自定义文案 | ☐ |

A5.1 是 PRD-06 的硬性要求：**不因 Agent 失败丢通知**。

---

## A6. 时钟降级与夜间规则（PRD-03）

| # | 操作 | 预期结果 | 判定 |
|---|---|---|---|
| A6.1 | `velaguard time unsync` | 打印 `时间可信: 否`、`夜间判定: 日间`、依据为「夜间加严规则已降级」 | ☐ |
| A6.2 | 紧接着 `velaguard sim impact --conf 0.9` | 只到**警告级**（没有按夜间加严直接升紧急） | ☐ |
| A6.3 | `velaguard log 3` | 该条记录带 `?` 标记（时间不可信） | ☐ |
| A6.4 | `velaguard time sync` | 恢复正常判定 | ☐ |

---

## A7. 事件日志（PRD-05）

| # | 操作 | 预期结果 | 判定 |
|---|---|---|---|
| A7.1 | `velaguard log 10` | 按时间倒序列出，含等级/类型/状态/持续时间/eventId | ☐ |
| A7.2 | `velaguard logjson 3` | 输出合法 JSON 数组，字段与 PRD-05 协议一致 | ☐ |
| A7.3 | `velaguard ui history` | 历史记录页显示最近 6 条 | ☐ |
| A7.4 | 重启模拟器后再 `velaguard log 10` | 记录**完整保留**（写入即落盘） | ☐ |
| A7.5 | `velaguard selftest` 后 `velaguard log 3` | 条数封顶 100，最旧记录被覆盖 | ☐ |

---

## A8. 识别引擎与性能（PRD-01）

```sh
velaguard bench 60
```

**预期**（模拟器 aarch64，已实测）：

```
识别链路性能标定（60 个 1 秒窗口，合成输入，不写事件日志）
  单窗口平均 : 约 306000 us
  单窗口峰值 : 约 335000 us
  实时率     : 约 30%
  预算       : < 500000 us（PRD-01）—— 通过
```

| # | 检查点 | 判定 |
|---|---|---|
| A8.1 | 峰值 < 500000 us，结论为「通过」 | ☐ |
| A8.2 | `velaguard bench` 后 `velaguard log` **条数不变**（空跑不写日志） | ☐ |

> 模拟器数字含 KASAN + QEMU 全系统模拟开销，通常是真机的数倍，
> 真机数字要在 B6 重新标定。

### A8.3 离线 wav 走真实识别链路（可选，需要把 wav 推进 `/data`）

```bash
# 宿主机先生成合成 wav
cd tests && python3 make_demo_wavs.py
# 通过 adb push / TF 卡 / 镜像打包把 .vgtest/wav/*.wav 放到设备 /data/wav/
```

```sh
velaguard feed /data/wav/alarm_beep.wav
velaguard log 3
```

**判定**：☐ 产生 `烟雾/燃气报警` 事件；换 `background.wav` 则**不产生任何事件**

> 模拟器上没有现成的 adb 工具时，这一项在**宿主机**用
> `tests/build/velaguard feed` 做等价验证（同一份源码），
> `tests/run_tests.sh` 第 3 节已覆盖。

---

## A9. UI 三页面与按键（PRD-04）

| # | 操作 | 预期结果 | 判定 |
|---|---|---|---|
| A9.1 | `velaguard ui home` | 监测首页：守护状态、音源、上传、时间同步、夜间判定、指示灯、最近事件、录入入口 | ☐ |
| A9.2 | `velaguard ui event` | 事件确认页：最高优先事件 + 倒计时 + 三种确认 + 排队事件 | ☐ |
| A9.3 | `velaguard ui history` | 历史记录页 | ☐ |
| A9.4 | `velaguard key page` 连按 3 次 | 三个页面循环切换 | ☐ |
| A9.5 | 首页时 `velaguard key enter` | 进入录入向导 `1/4 选择类别`，且提示「守护已暂停」 | ☐ |
| A9.6 | 向导中 `velaguard key page` | 在预设项之间切换（**没有自由文本输入**，符合"板端无键盘"） | ☐ |
| A9.7 | `velaguard key back` | 退出向导，守护自动恢复 | ☐ |

> LVGL LCD 界面默认关闭（`CONFIG_VELAGUARD_UI_LVGL`）。开启后画面内容与
> `velaguard ui` 完全一致，只是因 LVGL 默认字体不含 CJK，LCD 上显示 ASCII 文案。

---

## A10. 个性化录入（PRD-02）

需要 wav 素材（每条 ≥ 3 秒）。宿主机上可直接做：

```bash
cd tests
./build/velaguard enroll add help urgent 0 0 .vgtest/wav/shout_help.wav
./build/velaguard enroll list
```

| # | 检查点 | 判定 |
|---|---|---|
| A10.1 | 录入成功，提示「N 次采集，只存模板」 | ☐ |
| A10.2 | `enroll list` 显示类别/表达方式/短语/成员/采集次数/半径 | ☐ |
| A10.3 | 列表末尾打印「只保存声学模板，不保存任何原始录音」 | ☐ |
| A10.4 | 最小集自检提示还缺什么（成员/称呼/求救词/方言/未成对） | ☐ |
| A10.5 | `enroll del 0` 后立即从列表消失 | ☐ |
| A10.6 | `/data/velaguard/` 下**只有** `templates.bin`，没有任何 wav | ☐ |

---

## A11. 隐私红线（PRD-00）

| # | 操作 | 预期结果 | 判定 |
|---|---|---|---|
| A11.1 | 控制台注入含 `transcript` 的负载（见下） | 返回 **400**，错误信息「隐私红线：不允许上传字段 transcript」 | ☐ |
| A11.2 | 控制台注入非法 `eventType` | 返回 400 | ☐ |
| A11.3 | `git ls-files \| grep -E '\.(wav\|mp3\|pcm)$'` | 输出为空 | ☐ |
| A11.4 | 翻一遍 `velaguard logjson 10` 输出 | 无音频、无对话文本字段 | ☐ |

```bash
curl -X POST -H 'Content-Type: application/json' \
  -d '{"eventId":"x","deviceId":"d","eventType":"impact","level":"warning","transcript":"家里的对话"}' \
  http://127.0.0.1:8080/events
```

---

## A12. 模拟器测试结果记录表

| 组 | 用例数 | 通过 | 失败 | 备注 |
|---|---|---|---|---|
| A1 冒烟 | 4 | | | |
| A2 分级升级 | 6 | | | |
| A3 确认与并发 | 6 | | | |
| A4 远程通知 | 8 | | | |
| A5 Skill 回退 | 3 | | | |
| A6 时钟降级 | 4 | | | |
| A7 事件日志 | 5 | | | |
| A8 性能 | 3 | | | |
| A9 UI 与按键 | 7 | | | |
| A10 个性化录入 | 6 | | | |
| A11 隐私 | 4 | | | |
| **合计** | **56** | | | |

---

# B. 真机手动测试（Gemini-S1 R528）

> 前置：A 组全部通过。真机测试的重点是 **A 组测不了的东西**——
> 麦克风、扬声器、LED、按键、真实声音的识别率与误报率。

## B0. 烧录与基线（板到手第 1 天）

```bash
cp packages/ai_agent/defconfigs/gemini-s1/gemini-s1_defconfig \
  vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/defconfig
echo "CONFIG_LVX_USE_DEMO_CONTEST2026_121_VELAGUARD=y" \
  >> vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/defconfig
bash packages/ai_agent/fix_gemini_s1.sh
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/ \
  -e -Wno-error -j"$(nproc)"
```

| # | 检查点 | 判定 |
|---|---|---|
| B0.1 | 编译成功，产物生成 | ☐ |
| B0.2 | 烧录后串口稳定看到 `nsh>` | ☐ |
| B0.3 | `velaguard selftest` → 24/24 通过（**与模拟器结果一致**） | ☐ |
| B0.4 | 官方 `mini_memo` 的 PTT 录音/放音/屏幕能力正常（验证平台能力） | ☐ |
| B0.5 | `ls /dev` 确认 `/dev/audio/pcm0c`、`/dev/userleds`、`/dev/buttons` 实际路径 | ☐ |

B0.5 拿到的实际设备路径，如果与默认值不同，用 menuconfig 改：
`CONFIG_VELAGUARD_PCM_DEVICE` / `CONFIG_VELAGUARD_LED_DEVICE` /
`CONFIG_VELAGUARD_BUTTON_DEVICE`。

## B1. 硬件可用性自检

```sh
velaguard status
```

**看「硬件」行**：`LED 可用 / 扬声器 可用 / 按键 可用`

| # | 检查点 | 判定 |
|---|---|---|
| B1.1 | 三项都显示「可用」（若不可用，先解决 B0.5 的设备路径） | ☐ |
| B1.2 | 「采集状态」显示 `守护中（音源: 板载麦克风）`（需先 `velaguard start --mic`） | ☐ |

## B2. 录放并发能力标定（**板到手第一周必做**，PRD-01/04）

这是决定后续方案的关键测试：

```sh
velaguard start --mic
# 另一个串口/终端里播放提示音的同时观察采集是否中断
velaguard status          # 看「识别统计」的窗口数是否持续增长
```

| # | 检查点 | 结论 |
|---|---|---|
| B2.1 | 播放提示音期间，`识别统计` 的窗口计数**是否继续增长** | ☐ 是（支持全双工） ☐ 否（需时分） |
| B2.2 | 若支持：`velaguard duplex on`，再复测一次 B2.1 | ☐ |
| B2.3 | 若不支持：确认时分方案的单次中断 **≤ 2 秒**（`vg_capture_hold_for_playback` 已封顶） | ☐ |

**记录结论到 `docs/acceptance.md` 第三节。**

## B3. 本地提醒通道（PRD-04）

| # | 操作 | 预期 | 判定 |
|---|---|---|---|
| B3.1 | `velaguard sim water_flow --conf 0.85 --times 12 --interval 1000` | 扬声器出**短音一次**（提醒级） | ☐ |
| B3.2 | 升到警告级 | 扬声器**间隔音**，LED **黄色闪烁** | ☐ |
| B3.3 | 升到紧急级 | 扬声器**急促连续音**，LED **红色急闪** | ☐ |
| B3.4 | 无事件时 | LED **绿色呼吸**（守护中） | ☐ |
| B3.5 | 进入录入向导 | LED **蓝色常亮**，屏幕显示「守护已暂停」 | ☐ |
| B3.6 | 事件产生时 | LCD **背光自动唤醒**并切到事件确认页 | ☐ |

> `vg_led_write()` 目前用 userled 位掩码近似三色。若板上是 WS2812 单总线，
> 需要把这个函数换成真实 WS2812 驱动调用——**只改这一个函数**。

## B4. 按键（PRD-04）

先用 `ls /dev/buttons` 确认存在，然后逐键按下并观察：

| # | 物理键 | 预期动作 | 判定 |
|---|---|---|---|
| B4.1 | SW2 | 翻页 / 向导中"下一项" | ☐ |
| B4.2 | SW3 | 已处理 | ☐ |
| B4.3 | SW4 | 误报 | ☐ |
| B4.4 | SW5 | 稍后提醒 | ☐ |
| B4.5 | SW6 | 确认 / 进入录入向导 | ☐ |

实际键位与位掩码若不一致，改 `vg_input_poll()` 里的 bit → 动作映射
（当前假设 bit0..bit4 依次对应 SW2..SW6）。

| # | 检查点 | 判定 |
|---|---|---|
| B4.6 | 长按不会连发（上升沿去抖生效） | ☐ |
| B4.7 | 触摸屏若可用，能替代按键完成确认（**非依赖项，不通过不阻塞**） | ☐ |

## B5. 联网与时间（PRD-04/03）

```sh
wapi psk wlan0 <PASSWORD> 3
wapi essid wlan0 <SSID> 1
renew wlan0
ntpc start          # 或板端等效命令
velaguard time sync
velaguard status
```

| # | 检查点 | 判定 |
|---|---|---|
| B5.1 | 首页显示 Wi-Fi 与时间同步状态 | ☐ |
| B5.2 | NTP 同步后事件时间戳正确、`timeReliable` 为真 | ☐ |
| B5.3 | **断电重启且不联网**：`velaguard status` 显示「时间未同步」，夜间加严规则降级 | ☐ |
| B5.4 | Wi-Fi 凭证只在 `/data/velaguard/config.json`，`velaguard config` 不回显 | ☐ |

## B6. 真机性能与稳定性（PRD-01 验收）

```sh
velaguard bench 100
```

| # | 指标 | 预算 | 实测 | 判定 |
|---|---|---|---|---|
| B6.1 | 单窗口平均耗时 | — | ___ us | ☐ |
| B6.2 | 单窗口**峰值**耗时 | < 500000 us | ___ us | ☐ |
| B6.3 | 识别链路常驻内存 | ≤ 8 MB | ___ | ☐ |

```sh
velaguard start --mic
# 连续运行 2 小时后：
velaguard status
free
```

| # | 检查点 | 判定 |
|---|---|---|
| B6.4 | 连续 2 小时无崩溃、无 assert | ☐ |
| B6.5 | `free` 的 used 无持续增长（对比开始与结束） | ☐ |
| B6.6 | 窗口计数持续增长（采集没停） | ☐ |

## B7. 真实声音触发测试（PRD-08 核心验收）

每类 **≥ 20 次**，成功率 **≥ 80%**。建议两人配合，一人放声一人记录。

| 声景 | 触发方式 | 次数 | 成功 | 成功率 | 判定 |
|---|---|---|---|---|---|
| 烟雾报警 | 真实烟感测试键 / 手机播放报警音 | 20 | | | ☐ ≥80% |
| 持续水流 | 真实开水龙头 / 淋浴 | 20 | | | ☐ ≥80% |
| 破碎撞击 | 摔纸箱、敲木板（**不要用真玻璃**） | 20 | | | ☐ ≥80% |
| 呻吟/痛苦叫声 | 成员拟音 | 20 | | | ☐ ≥80% |
| 姓名/求救词 | 成员真实呼喊（先完成 B8 录入） | 20 | | | ☐ ≥80% |

每次触发后用 `velaguard log 1` 确认事件类型与等级是否正确。

### B7.6 误报测试（≥ 30 分钟）

场景：电视声、音乐、正常交谈、**平静喊名字**、做饭声。

| # | 检查点 | 判定 |
|---|---|---|
| B7.6a | 30 分钟内高风险误报（警告级及以上）**≤ 1 次** | ☐ |
| B7.6b | 平静喊名字**不产生远程通知**（可以有提醒级） | ☐ |
| B7.6c | 记录每次误报的类型与当时环境，写进验收报告 | ☐ |

## B8. 个性化录入实机验证（PRD-02）

用板载按键走完整向导，录入 PRD-02 规定的最小集：

- ≥ 2 个家庭成员声音
- ≥ 2 个姓名/亲属称呼
- ≥ 3 个求救短语
- ≥ 1 种方言版本
- 每条短语**平静 + 急促成对**

```sh
velaguard enroll list      # 看最小集自检提示
```

| # | 检查点 | 判定 |
|---|---|---|
| B8.1 | 全程用按键完成，无需键盘 | ☐ |
| B8.2 | 每条采集 3–5 次，界面显示进度 | ☐ |
| B8.3 | 录入期间屏幕明确显示「守护已暂停」，退出后自动恢复 | ☐ |
| B8.4 | `enroll list` 提示「录入最小集已满足」 | ☐ |
| B8.5 | 重录同一条会覆盖旧模板（不重复占位） | ☐ |
| B8.6 | 删除后立即失效（再喊不再命中） | ☐ |
| B8.7 | `/data/velaguard/` 下无任何音频文件 | ☐ |

### B8.8 识别矩阵（PRD-08）

| 说话者 | 距离 | 语言 | 背景 | 召回 | 判定 |
|---|---|---|---|---|---|
| 成员 A | 近(1m) | 普通话 | 安静 | | ☐ ≥0.85 |
| 成员 A | 远(4m) | 普通话 | 安静 | | ☐ |
| 成员 A | 近 | 方言 | 安静 | | ☐ |
| 成员 A | 近 | 普通话 | 电视声 | | ☐ |
| 成员 B | 近 | 普通话 | 安静 | | ☐ ≥0.85 |
| 成员 B | 远 | 普通话 | 电视声 | | ☐ |

## B9. 音频通路互斥（PRD-01/04）

| # | 操作 | 预期 | 判定 |
|---|---|---|---|
| B9.1 | 守护运行中启动 ai_agent PTT 对话 | 屏幕显示「守护已暂停」，采集停止 | ☐ |
| B9.2 | 退出对话 | 守护**自动恢复**，窗口计数重新增长 | ☐ |
| B9.3 | 进入录入向导 | 同 B9.1 | ☐ |
| B9.4 | 退出录入 | 同 B9.2 | ☐ |

## B10. 异常与降级场景

| # | 场景 | 预期 | 判定 |
|---|---|---|---|
| B10.1 | 拔掉 Wi-Fi / 关控制台 | 本地识别、提醒、确认、日志**全部照常**；通知入队 | ☐ |
| B10.2 | 恢复网络后 `velaguard flush` | 补发成功且无重复 | ☐ |
| B10.3 | 删除 `/data/agent/skills/anling-home-safety` | 通知照常发出（内置模板文案） | ☐ |
| B10.4 | 控制台 + Agent + LLM 全不可用 | 端侧安全能力完整（这是 PRD-08 场景 8） | ☐ |
| B10.5 | 日志写满 100 条 | 正确环形覆盖，无崩溃 | ☐ |
| B10.6 | 突然断电再上电 | 事件日志、模板、待发送队列全部恢复 | ☐ |

## B11. 真机测试结果记录表

| 组 | 用例数 | 通过 | 失败 | 备注 |
|---|---|---|---|---|
| B0 烧录基线 | 5 | | | |
| B1 硬件自检 | 2 | | | |
| B2 录放并发 | 3 | | | **结论要写进验收文档** |
| B3 提醒通道 | 6 | | | |
| B4 按键 | 7 | | | |
| B5 联网与时间 | 4 | | | |
| B6 性能稳定性 | 6 | | | |
| B7 真实触发 + 误报 | 8 | | | 核心验收 |
| B8 个性化录入 | 13 | | | 核心验收 |
| B9 通路互斥 | 4 | | | |
| B10 异常降级 | 6 | | | |
| **合计** | **64** | | | |

---

# C. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| NSH 里命令被截断成两条 | NSH 单行长度限制（约 80 字符） | 拆成多条命令，或缩短参数 |
| `未找到待确认事件` | 事件已被确认/关闭，或已超过静默期 | 先 `velaguard ui event` 确认还有活跃事件 |
| `velaguard status` 上传一直离线 | 控制台地址不对 / 模拟器 NAT | 模拟器访问宿主机用 `10.0.2.2`；真机用局域网 IP |
| `事件日志文件不可用` | `/data` 未挂载或只读 | 降级为内存日志，本地告警不受影响；检查分区表 |
| `velaguard feed` 报「仅支持 16bit wav」 | wav 格式不对 | 先跑 `model/prepare_dataset.py` 转成 16kHz/16bit 单声道 |
| LED / 按键显示不可用 | 设备路径与默认值不符 | `ls /dev` 确认后改 menuconfig 里的三个路径 |
| LCD 上是英文 | LVGL 默认字体不含 CJK | 内置 CJK 子集字体后把 `vg_ui_render(..., ascii=false)` |
| 模拟器起不来、Qt 报错 | 无图形环境 | 加 `-no-window` |
| 主机测试 curl 全部 502 | 开发机设了 http_proxy | `export no_proxy=127.0.0.1,localhost`（`run_tests.sh` 已内置处理） |

---

# D. 每次改动后的最小回归

改任何代码后，至少跑这三条再继续：

```bash
cd tests && make test                    # 主机侧 18 项
```

```sh
velaguard selftest                       # 模拟器/真机 24 项
velaguard bench 60                       # 性能没有退化
```

三条全绿再做手动用例，否则先修。
