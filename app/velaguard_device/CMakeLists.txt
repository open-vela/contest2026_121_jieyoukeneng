# 安聆 VelaGuard - CMake 构建
if(CONFIG_LVX_USE_DEMO_CONTEST2026_121_VELAGUARD)

  nuttx_add_application(
    NAME velaguard
    STACKSIZE ${CONFIG_VELAGUARD_STACKSIZE}
    PRIORITY ${CONFIG_VELAGUARD_PRIORITY}
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include
    SRCS
      velaguard_main.c
      velaguard_daemon.c
      common/vg_json.c
      common/vg_types.c
      common/vg_time.c
      common/vg_config.c
      event_log/vg_event_log.c
      event_state_machine/vg_event_sm.c
      audio_detector/vg_feature.c
      audio_detector/vg_classifier.c
      audio_detector/vg_model_weights.c
      audio_detector/vg_capture.c
      audio_detector/vg_detector.c
      enrollment/vg_enroll.c
      uploader/vg_http.c
      uploader/vg_transport.c
      uploader/vg_notifier.c
      uploader/vg_agent.c
      uploader/vg_uploader.c
      hardware_adapter/vg_indicator.c
      hardware_adapter/vg_input.c
      hardware_adapter/vg_ui.c
      hardware_adapter/vg_ui_lvgl.c)

endif()
