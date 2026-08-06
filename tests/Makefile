############################################################################
# 安聆 VelaGuard - 主机侧构建与测试
#
# 端侧核心模块（协议、状态机、事件日志、特征、分类器、模板匹配、通知）
# 只依赖 C 标准库与 POSIX，因此可以在开发机上直接编译运行，
# 用于快速回归与 CI，无需等待 openvela 全量构建。
#
#   make            编译主机版可执行文件 build/velaguard
#   make test       跑内置验收自检 + 特征一致性检查
#   make clean
############################################################################

APP_DIR := ../app/velaguard_device
BUILD   := build

CC      ?= gcc
CFLAGS  += -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -D_GNU_SOURCE -I$(APP_DIR)/include
LDFLAGS += -lm -lpthread

SRCS := \
  $(APP_DIR)/common/vg_json.c \
  $(APP_DIR)/common/vg_types.c \
  $(APP_DIR)/common/vg_time.c \
  $(APP_DIR)/common/vg_config.c \
  $(APP_DIR)/event_log/vg_event_log.c \
  $(APP_DIR)/event_state_machine/vg_event_sm.c \
  $(APP_DIR)/audio_detector/vg_feature.c \
  $(APP_DIR)/audio_detector/vg_classifier.c \
  $(APP_DIR)/audio_detector/vg_model_weights.c \
  $(APP_DIR)/audio_detector/vg_capture.c \
  $(APP_DIR)/audio_detector/vg_detector.c \
  $(APP_DIR)/enrollment/vg_enroll.c \
  $(APP_DIR)/uploader/vg_http.c \
  $(APP_DIR)/uploader/vg_transport.c \
  $(APP_DIR)/uploader/vg_notifier.c \
  $(APP_DIR)/uploader/vg_agent.c \
  $(APP_DIR)/uploader/vg_uploader.c \
  $(APP_DIR)/hardware_adapter/vg_indicator.c \
  $(APP_DIR)/hardware_adapter/vg_input.c \
  $(APP_DIR)/hardware_adapter/vg_diagnostics.c \
  $(APP_DIR)/hardware_adapter/vg_ui.c \
  $(APP_DIR)/hardware_adapter/vg_wifi.c \
  $(APP_DIR)/velaguard_daemon.c

MAIN     := $(APP_DIR)/velaguard_main.c
FEATDUMP := tools/vg_featdump.c

TARGET   := $(BUILD)/velaguard
DUMPBIN  := $(BUILD)/vg_featdump

.PHONY: all test clean

all: $(TARGET) $(DUMPBIN)

$(TARGET): $(SRCS) $(MAIN)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(DUMPBIN): $(SRCS) $(FEATDUMP)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: all
	@bash run_tests.sh

clean:
	rm -rf $(BUILD) .vgtest
