############################################################################
# 安聆 VelaGuard - Makefile
############################################################################

include $(APPDIR)/Make.defs

ifeq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_121_VELAGUARD),y)

PROGNAME  = velaguard
PRIORITY  = $(CONFIG_VELAGUARD_PRIORITY)
STACKSIZE = $(CONFIG_VELAGUARD_STACKSIZE)
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_121_VELAGUARD)

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/demos/contest2026_121_velaguard/include

CSRCS  = common/vg_json.c
CSRCS += common/vg_types.c
CSRCS += common/vg_time.c
CSRCS += common/vg_config.c
CSRCS += event_log/vg_event_log.c
CSRCS += event_state_machine/vg_event_sm.c
CSRCS += audio_detector/vg_feature.c
CSRCS += audio_detector/vg_classifier.c
CSRCS += audio_detector/vg_model_weights.c
CSRCS += audio_detector/vg_capture.c
CSRCS += audio_detector/vg_detector.c
CSRCS += enrollment/vg_enroll.c
CSRCS += uploader/vg_http.c
CSRCS += uploader/vg_transport.c
CSRCS += uploader/vg_notifier.c
CSRCS += uploader/vg_agent.c
CSRCS += uploader/vg_uploader.c
CSRCS += hardware_adapter/vg_indicator.c
CSRCS += hardware_adapter/vg_input.c
CSRCS += hardware_adapter/vg_ui.c
CSRCS += hardware_adapter/vg_ui_lvgl.c
CSRCS += velaguard_daemon.c

MAINSRC = velaguard_main.c

endif

include $(APPDIR)/Application.mk
