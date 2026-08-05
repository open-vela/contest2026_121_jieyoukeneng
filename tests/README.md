# tests/ —— 主机侧回归与验收

端侧核心模块（协议、状态机、事件日志、特征、分类器、模板匹配、通知、上传）
只依赖 C 标准库与 POSIX，因此可以直接在开发机上编译运行。这让每次改动
都能在几秒内回归，而不用等 openvela 全量构建（约 2 分钟）+ 模拟器启动（约 50 秒）。

## 用法

```bash
cd tests
make            # 编译 build/velaguard（主机版）与 build/vg_featdump
make test       # 跑完整回归（21 项断言，环境完整时）
make clean
```

依赖：`gcc`、`python3` + `numpy`（特征一致性与 wav 生成）、`node`（控制台闭环）。
缺 numpy 或 node 时对应小节自动跳过，其余照跑。

## 覆盖内容

| 小节 | 内容 |
|---|---|
| 1 | 端侧内置验收自检（`velaguard selftest`，25 项断言，覆盖 PRD-08 可自动化场景） |
| 2 | **端侧 C 特征 与 训练脚本 Python 特征逐维一致性**（最大绝对误差 3.0e-05） |
| 3 | 真实识别链路：wav → 特征 → int8 推理 → 观测 → 状态机 → 事件；含背景噪声负样本 |
| 4 | 事件日志断电重启不丢失 + 落盘校验 |
| 5 | 控制台协议闭环：上传 / 幂等补发 / 命令拉取 / 接收回执 / 结果 ACK / 隐私字段拒绝 / 协议校验 / 断网恢复 |
| 6 | 隐私自检：Git 跟踪文件中无音频、无真实凭证 |

第 2 项是最关键的一条：两侧特征只要有一维对不上，电脑端训练出来的
指标在板上就不成立。这条不过，其余指标都不可信。

## 文件

| 文件 | 作用 |
|---|---|
| `Makefile` | 主机侧构建 |
| `run_tests.sh` | 回归主脚本 |
| `check_feature_parity.py` | C 与 Python 特征一致性校验 |
| `make_demo_wavs.py` | 生成演示/回归用合成 wav（写入 `.vgtest/wav/`，不进 Git） |
| `tools/vg_featdump.c` | 特征转储工具，支持 `--classify` 直接看分类结果 |

## 主机版可执行文件也能当演示工具

`build/velaguard` 与板端是**同一份源码**，所有子命令都可用：

```bash
export VELAGUARD_DATA_DIR=$PWD/.vgtest/data
export VELAGUARD_CONSOLE_HOST=127.0.0.1
export VELAGUARD_SKILL_DIR=$PWD/../agent_skill/anling-home-safety

./build/velaguard selftest
./build/velaguard bench 100
./build/velaguard sim name_call_help --phrase 救命 --urgency 0.9 --times 3
./build/velaguard ui event
./build/velaguard feed .vgtest/wav/alarm_beep.wav
```

环境变量 `VELAGUARD_DATA_DIR` / `VELAGUARD_CONSOLE_HOST` /
`VELAGUARD_CONSOLE_PORT` / `VELAGUARD_DEVICE_ID` / `VELAGUARD_SKILL_DIR`
只用于主机侧联调；板端不设置这些变量，行为完全由
`/data/velaguard/config.json` 决定。
