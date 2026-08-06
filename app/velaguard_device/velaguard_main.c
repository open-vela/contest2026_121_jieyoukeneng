/****************************************************************************
 * 安聆 VelaGuard - NSH 命令入口
 *
 * 一个命令覆盖演示与联调所需的全部操作：守护、模拟注入、确认、录入、
 * 日志、离线 wav 识别、配置与自检。
 ****************************************************************************/

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "velaguard/vg_agent.h"
#include "velaguard/vg_capture.h"
#include "velaguard/vg_classifier.h"
#include "velaguard/vg_config.h"
#include "velaguard/vg_daemon.h"
#include "velaguard/vg_detector.h"
#include "velaguard/vg_diagnostics.h"
#include "velaguard/vg_enroll.h"
#include "velaguard/vg_event_log.h"
#include "velaguard/vg_event_sm.h"
#include "velaguard/vg_feature.h"
#include "velaguard/vg_indicator.h"
#include "velaguard/vg_input.h"
#include "velaguard/vg_indicator.h"
#include "velaguard/vg_json.h"
#include "velaguard/vg_notifier.h"
#include "velaguard/vg_time.h"
#include "velaguard/vg_ui.h"
#include "velaguard/vg_uploader.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void vg_usage(void)
{
  printf(
    "安聆 VelaGuard —— 无摄像头家庭异常声景 AI Agent\n"
    "\n"
    "用法: velaguard <子命令> [参数]\n"
    "\n"
    "守护与状态\n"
    "  start [--mic|--wav <file>]   启动守护（缺省无音频源，用模拟注入演示）\n"
    "  stop                         停止守护\n"
    "  status                       运行状态、性能与硬件可用性\n"
    "  config                       打印当前配置（不回显任何凭证）\n"
    "\n"
    "事件与确认\n"
    "  sim <type> [选项]            注入模拟观测，走完全相同的状态机\n"
    "       type: alarm_beep|water_flow|impact|distress_voice|name_call_help\n"
    "       --conf <0~1>  --phrase <短语>  --person <标签>\n"
    "       --urgency <0~1>  --repeat <n>  --times <n>  --interval <ms>\n"
    "  ack [<eventId>] <handled|false_alarm|snooze>\n"
    "  key <page|handled|false|snooze|enter|back|up|down>\n"
    "  ui [home|event|history]      打印当前页面（LCD 同款内容）\n"
    "  log [n]                      打印最近 n 条事件（默认 10）\n"
    "  logjson [n]                  以 JSON 数组输出（联调用）\n"
    "\n"
    "识别与录入\n"
    "  feed <file.wav>              离线跑真实识别链路（模拟器线验证）\n"
    "  enroll list|del <idx>|clear\n"
    "  enroll add <name|help|dialect> <calm|urgent> <短语idx> <成员idx> <wav>\n"
    "\n"
    "运维与自检\n"
    "  time <sync|unsync>           标记时间是否可信（夜间规则降级演示）\n"
    "  duplex <on|off>              标定录放并发能力\n"
    "  test <mic|speaker|network|all> 板端硬件与网络自检\n"
    "  notify test                  生成一条测试通知并上传\n"
    "  notify --json '<事件JSON>'    Skill 文案回灌（见 agent_skill/）\n"
    "  flush                        立即重试所有待发送通知\n"
    "  selftest                     跑一遍内置验收场景（不依赖硬件）\n"
    "  bench [n]                    识别链路性能标定（默认 50 个窗口）\n");
}

static float vg_argf(int argc, char **argv, const char *key, float def)
{
  int i;

  for (i = 0; i < argc - 1; i++)
    {
      if (strcmp(argv[i], key) == 0)
        {
          return (float)atof(argv[i + 1]);
        }
    }

  return def;
}

static int vg_argi(int argc, char **argv, const char *key, int def)
{
  int i;

  for (i = 0; i < argc - 1; i++)
    {
      if (strcmp(argv[i], key) == 0)
        {
          return atoi(argv[i + 1]);
        }
    }

  return def;
}

static const char *vg_args(int argc, char **argv, const char *key,
                           const char *def)
{
  int i;

  for (i = 0; i < argc - 1; i++)
    {
      if (strcmp(argv[i], key) == 0)
        {
          return argv[i + 1];
        }
    }

  return def;
}

static bool vg_hasflag(int argc, char **argv, const char *key)
{
  int i;

  for (i = 0; i < argc; i++)
    {
      if (strcmp(argv[i], key) == 0)
        {
          return true;
        }
    }

  return false;
}

static void vg_print_page(void)
{
  static char buf[1280];

  if (vg_ui_render(buf, sizeof(buf), false) > 0)
    {
      printf("%s", buf);
    }
}

static int vg_cmd_sim(int argc, char **argv)
{
  vg_event_type_t type;
  const char *phrase;
  const char *person;
  float conf;
  float urgency;
  int repeat;
  int times;
  int interval;
  int i;

  if (argc < 3 || vg_event_type_parse(argv[2], &type) < 0)
    {
      printf("用法: velaguard sim <alarm_beep|water_flow|impact|"
             "distress_voice|name_call_help> [选项]\n");
      return 1;
    }

  conf     = vg_argf(argc, argv, "--conf", 0.9f);
  urgency  = vg_argf(argc, argv, "--urgency",
                     type == VG_EVT_NAME_CALL_HELP ? 0.2f : 0.8f);
  repeat   = vg_argi(argc, argv, "--repeat", 1);
  times    = vg_argi(argc, argv, "--times", 1);
  interval = vg_argi(argc, argv, "--interval", 0);
  phrase   = vg_args(argc, argv, "--phrase", "");
  person   = vg_args(argc, argv, "--person", "");

  vg_daemon_init();

  for (i = 0; i < times; i++)
    {
      vg_sm_inject(type, conf, phrase, person, urgency, (uint16_t)repeat);

      if (interval > 0 && i + 1 < times)
        {
          if (vg_daemon_running())
            {
              usleep((useconds_t)interval * 1000);
            }
          else
            {
              /* 未启动守护时用虚拟时钟推进，保证阈值规则可复现 */

              vg_time_use_virtual(true);
              vg_time_virtual_advance((uint64_t)interval);
            }
        }

      vg_sm_tick();
    }

  printf("已注入 %d 次 %s 观测（置信度 %.2f 急促度 %.2f）\n",
         times, vg_event_type_str(type), (double)conf, (double)urgency);
  vg_ui_set_page(VG_PAGE_EVENT);
  vg_print_page();

  /* 给上传模块一个机会发送事件（独立进程模式下 daemon 不会自动 tick） */

  for (i = 0; i < 5; i++)
    {
      vg_uploader_tick();
      usleep(200000);
    }

  return 0;
}

static int vg_cmd_ack(int argc, char **argv)
{
  vg_local_status_t st;
  const char *id = NULL;
  const char *act;
  int ret;

  if (argc < 3)
    {
      printf("用法: velaguard ack [<eventId>] <handled|false_alarm|snooze>\n");
      return 1;
    }

  if (argc >= 4)
    {
      id = argv[2];
      act = argv[3];
    }
  else
    {
      act = argv[2];
    }

  if (strcmp(act, "handled") == 0)
    {
      st = VG_STATUS_HANDLED;
    }
  else if (strcmp(act, "false_alarm") == 0 || strcmp(act, "false") == 0)
    {
      st = VG_STATUS_FALSE_ALARM;
    }
  else if (strcmp(act, "snooze") == 0)
    {
      st = VG_STATUS_SNOOZED;
    }
  else
    {
      printf("未知确认动作: %s\n", act);
      return 1;
    }

  ret = vg_sm_ack(id, st);
  if (ret == -1)
    {
      printf("未找到待确认事件\n");
      return 1;
    }

  if (ret == -2)
    {
      printf("稍后提醒次数已用尽（上限 %" PRIu32 " 次），"
             "本次倒计时到期将按无人响应升级\n",
             vg_config()->snooze_max_count);
      return 1;
    }

  printf("已确认: %s\n", vg_status_cn(st));
  vg_print_page();
  return 0;
}

static int vg_cmd_log(int argc, char **argv, bool json)
{
  int n = (argc >= 3) ? atoi(argv[2]) : 10;
  int total = vg_event_log_count();
  int i;

  if (n <= 0 || n > total)
    {
      n = total;
    }

  if (json)
    {
      static char buf[8192];

      if (vg_event_log_to_json(n, buf, sizeof(buf)) > 0)
        {
          printf("%s\n", buf);
        }

      return 0;
    }

  printf("本地事件日志 %d/%d 条（累计写入 %" PRIu32 "）\n",
         total, VG_EVENT_LOG_CAPACITY, vg_event_log_total());

  for (i = 0; i < n; i++)
    {
      vg_safety_event_t evt;
      char ts[VG_TIMESTR_LEN];

      if (vg_event_log_get(i, &evt) < 0)
        {
          break;
        }

      vg_format_time(evt.started_at, ts, sizeof(ts));
      printf("%-2d %s %s [%s] %-14s %-8s %3ds %s\n",
             i, ts, evt.time_reliable ? " " : "?",
             vg_level_cn(evt.level), vg_event_type_cn(evt.type),
             vg_status_cn(evt.local_status), (int)evt.duration_sec,
             evt.event_id);
    }

  return 0;
}

static int vg_cmd_enroll(int argc, char **argv)
{
  if (argc < 3 || strcmp(argv[2], "list") == 0)
    {
      char why[64];
      int n = vg_enroll_count();
      int i;

      printf("已录入模板 %d 条（上限 %d）\n", n, VG_TEMPLATE_MAX);
      for (i = 0; i < n; i++)
        {
          vg_template_t t;

          if (vg_enroll_get(i, &t) < 0)
            {
              break;
            }

          printf("%-2d %-8s %-6s %-14s %-12s 采集%u次 半径%.3f\n", i,
                 t.kind == VG_TPL_NAME ? "姓名/称呼" :
                 t.kind == VG_TPL_HELP ? "求救词" : "方言",
                 t.style == VG_TPL_URGENT ? "急促" : "平静",
                 t.phrase, t.person, (unsigned)t.takes, (double)t.radius);
        }

      vg_enroll_min_set_ready(why, sizeof(why));
      printf("最小集自检: %s\n", why);
      printf("提示: 只保存声学模板，不保存任何原始录音\n");
      return 0;
    }

  if (strcmp(argv[2], "del") == 0 && argc >= 4)
    {
      if (vg_enroll_delete(atoi(argv[3])) < 0)
        {
          printf("删除失败: 索引无效\n");
          return 1;
        }

      printf("已删除（立即生效且不可恢复）\n");
      return 0;
    }

  if (strcmp(argv[2], "clear") == 0)
    {
      vg_enroll_clear();
      printf("已清空全部模板\n");
      return 0;
    }

  if (strcmp(argv[2], "add") == 0 && argc >= 8)
    {
      static int16_t pcm[VG_WINDOW_SAMPLES];
      vg_tpl_kind_t kind;
      vg_tpl_style_t style;
      int phrase_idx;
      int person_idx;
      int takes = 0;
      int slot;

      if (strcmp(argv[3], "name") == 0)
        {
          kind = VG_TPL_NAME;
        }
      else if (strcmp(argv[3], "help") == 0)
        {
          kind = VG_TPL_HELP;
        }
      else
        {
          kind = VG_TPL_DIALECT;
        }

      style = (strcmp(argv[4], "urgent") == 0) ? VG_TPL_URGENT : VG_TPL_CALM;
      phrase_idx = atoi(argv[5]);
      person_idx = atoi(argv[6]);

      /* 录入期间守护采集暂停（音频通路互斥） */

      vg_capture_pause("守护已暂停：录入中");

      vg_enroll_begin(kind, style,
                      vg_enroll_preset_phrase(kind, phrase_idx),
                      vg_enroll_preset_person(person_idx));

      if (vg_capture_open(VG_SRC_WAV, argv[7]) < 0)
        {
          vg_enroll_cancel();
          vg_capture_resume();
          return 1;
        }

      /* 录入采集不受暂停影响：这里直接从 wav 读，模拟 3~5 次采集 */

      vg_capture_resume();

      while (takes < VG_ENROLL_MAX_TAKES)
        {
          float feat[VG_FEATURE_DIM];
          int got = vg_capture_read(pcm, VG_WINDOW_SAMPLES);

          if (got < VG_FRAME_LEN)
            {
              break;
            }

          if (vg_feature_extract(pcm, (size_t)got, feat) < 0)
            {
              break;
            }

          vg_enroll_feed(feat);
          takes++;
        }

      vg_capture_close();

      if (takes < VG_ENROLL_MIN_TAKES)
        {
          printf("采集不足 %d 次（wav 至少需 %d 秒），已取消\n",
                 VG_ENROLL_MIN_TAKES, VG_ENROLL_MIN_TAKES);
          vg_enroll_cancel();
          return 1;
        }

      slot = vg_enroll_commit();
      if (slot < 0)
        {
          printf("保存失败(%d)\n", slot);
          return 1;
        }

      printf("已录入: %s / %s / %s（%d 次采集，只存模板）\n",
             vg_enroll_preset_phrase(kind, phrase_idx),
             vg_enroll_preset_person(person_idx),
             style == VG_TPL_URGENT ? "急促" : "平静", takes);
      return 0;
    }

  printf("用法: velaguard enroll list|del <idx>|clear\n"
         "      velaguard enroll add <name|help|dialect> <calm|urgent>"
         " <短语idx> <成员idx> <wav>\n");
  return 1;
}

static int vg_cmd_selftest(void);

static int vg_cmd_test(int argc, char **argv)
{
  const char *kind = argc >= 3 ? argv[2] : "all";

  if (strcmp(kind, "mic") != 0 && strcmp(kind, "speaker") != 0 &&
      strcmp(kind, "network") != 0 && strcmp(kind, "all") != 0)
    {
      printf("用法: velaguard test <mic|speaker|network|all>\n");
      return 1;
    }

  return vg_diagnostics_run(strcmp(kind, "mic") == 0 ? VG_DIAG_MIC :
                            strcmp(kind, "speaker") == 0 ? VG_DIAG_SPEAKER :
                            strcmp(kind, "network") == 0 ? VG_DIAG_NETWORK :
                            VG_DIAG_ALL) == 0 ? 0 : 1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  const char *cmd;

  if (argc < 2)
    {
      vg_usage();
      return 0;
    }

  cmd = argv[1];

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0)
    {
      vg_usage();
      return 0;
    }

  if (strcmp(cmd, "start") == 0)
    {
      vg_source_t src = VG_SRC_NONE;
      const char *path = NULL;

      if (vg_hasflag(argc, argv, "--mic"))
        {
          src = VG_SRC_MIC;
        }
      else if (vg_args(argc, argv, "--wav", NULL) != NULL)
        {
          src = VG_SRC_WAV;
          path = vg_args(argc, argv, "--wav", NULL);
        }

      return vg_daemon_start(src, path);
    }

  if (strcmp(cmd, "stop") == 0)
    {
      return vg_daemon_stop();
    }

  vg_daemon_init();

  if (strcmp(cmd, "status") == 0)
    {
      vg_daemon_status();
      return 0;
    }

  if (strcmp(cmd, "test") == 0)
    {
      return vg_cmd_test(argc, argv);
    }

  if (strcmp(cmd, "config") == 0)
    {
      vg_config_dump();
      return 0;
    }

  if (strcmp(cmd, "sim") == 0)
    {
      return vg_cmd_sim(argc, argv);
    }

  if (strcmp(cmd, "ack") == 0)
    {
      return vg_cmd_ack(argc, argv);
    }

  if (strcmp(cmd, "key") == 0)
    {
      vg_action_t act;

      if (argc < 3)
        {
          printf("用法: velaguard key <page|handled|false|snooze|enter|"
                 "back|up|down>\n");
          return 1;
        }

      act = vg_input_parse(argv[2]);
      if (act == VG_ACT_NONE)
        {
          printf("未知按键动作: %s\n", argv[2]);
          return 1;
        }

      vg_input_dispatch(act);
      vg_print_page();
      return 0;
    }

  if (strcmp(cmd, "ui") == 0)
    {
      if (argc >= 3)
        {
          if (strcmp(argv[2], "home") == 0)
            {
              vg_ui_set_page(VG_PAGE_HOME);
            }
          else if (strcmp(argv[2], "event") == 0)
            {
              vg_ui_set_page(VG_PAGE_EVENT);
            }
          else if (strcmp(argv[2], "history") == 0)
            {
              vg_ui_set_page(VG_PAGE_HISTORY);
            }
        }

      vg_print_page();
      return 0;
    }

  if (strcmp(cmd, "log") == 0)
    {
      return vg_cmd_log(argc, argv, false);
    }

  if (strcmp(cmd, "logjson") == 0)
    {
      return vg_cmd_log(argc, argv, true);
    }

  if (strcmp(cmd, "feed") == 0)
    {
      int windows;

      if (argc < 3)
        {
          printf("用法: velaguard feed <file.wav>\n");
          return 1;
        }

      windows = vg_detector_run_file(argv[2]);
      if (windows < 0)
        {
          printf("识别失败: 无法打开 %s\n", argv[2]);
          return 1;
        }

      printf("已处理 %d 个 1 秒窗口\n", windows);
      vg_daemon_status();
      return 0;
    }

  if (strcmp(cmd, "enroll") == 0)
    {
      return vg_cmd_enroll(argc, argv);
    }

  if (strcmp(cmd, "time") == 0)
    {
      if (argc >= 3 && strcmp(argv[2], "sync") == 0)
        {
          vg_time_set_reliable(true);
        }
      else if (argc >= 3 && strcmp(argv[2], "unsync") == 0)
        {
          vg_time_set_reliable(false);
        }

      printf("时间可信: %s\n", vg_time_reliable() ? "是" : "否");
      printf("夜间判定: %s\n", vg_is_night() ? "夜间" : "日间");
      printf("判定依据: %s\n", vg_night_reason());
      return 0;
    }

  if (strcmp(cmd, "duplex") == 0)
    {
      vg_indicator_set_full_duplex(argc >= 3 &&
                                   strcmp(argv[2], "on") == 0);
      return 0;
    }

  if (strcmp(cmd, "notify") == 0)
    {
      vg_safety_event_t evt;

      /* Skill 回灌约定：velaguard notify --json '<事件 JSON, 含 advice>'
       * ai_agent 生成文案后用本命令把结果交回 notifier 发送。
       */

      if (argc >= 4 && strcmp(argv[2], "--json") == 0)
        {
          char advice[VG_ADVICE_LEN];

          if (vg_event_from_json(argv[3], &evt) < 0 ||
              vg_event_validate(&evt) != 0)
            {
              printf("事件 JSON 非法\n");
              return 1;
            }

          advice[0] = '\0';
          if (vg_json_get_str(argv[3], "advice", advice,
                              sizeof(advice)) == 0 && advice[0] != '\0')
            {
              printf("采用 Agent 生成的文案: %s\n", advice);
            }
          else
            {
              printf("未附带 advice，使用 Skill 规则表/内置模板生成\n");
            }

          vg_uploader_enqueue_advice(&evt, evt.upload_reason,
                                     advice[0] != '\0' ? advice : NULL);
          /* NSH 单次命令没有守护循环，由命令 owner 显式触发一次网络任务；
           * 状态机回调本身不会走到这里，也不会同步联网。 */
          vg_uploader_flush();
          printf("已入队，待发送 %d 条\n", vg_uploader_pending());
          return 0;
        }

      memset(&evt, 0, sizeof(evt));
      snprintf(evt.event_id, sizeof(evt.event_id), "evt_test");
      vg_strlcpy(evt.device_id, vg_config()->device_id, sizeof(evt.device_id));
      evt.type = VG_EVT_ALARM_BEEP;
      evt.level = VG_LEVEL_EMERGENCY;
      evt.confidence = 0.95f;
      evt.started_at = vg_wall_sec();
      evt.duration_sec = 6;
      evt.local_status = VG_STATUS_NO_RESPONSE;
      evt.time_reliable = vg_time_reliable();
      evt.night = vg_is_night();
      snprintf(evt.summary, sizeof(evt.summary),
               "通知链路测试事件（非真实告警）");

      vg_uploader_enqueue(&evt, VG_UPLOAD_MANUAL_TEST);
      vg_uploader_flush();
      printf("测试通知已入队，待发送 %d 条\n", vg_uploader_pending());
      return 0;
    }

  if (strcmp(cmd, "flush") == 0)
    {
      vg_uploader_flush();
      printf("待发送 %d 条，连接状态: %s\n",
             vg_uploader_pending(),
             vg_uploader_online() ? "在线" : "离线");
      return 0;
    }

  if (strcmp(cmd, "bench") == 0)
    {
      /* PRD-01 性能验收：单个 1 秒窗口端到端处理 < 500ms。
       * 用确定性合成信号驱动完整链路（特征 + 两个模型 + 模板匹配），
       * 只测算力，不涉及任何真实音频。
       */

      static int16_t pcm[VG_WINDOW_SAMPLES];
      vg_detector_stats_t st;
      uint32_t seed = 12345;
      int n = (argc >= 3) ? atoi(argv[2]) : 50;
      int i;

      if (n <= 0 || n > 1000)
        {
          n = 50;
        }

      for (i = 0; i < VG_WINDOW_SAMPLES; i++)
        {
          seed = seed * 1103515245u + 12345u;
          pcm[i] = (int16_t)((int32_t)((seed >> 16) & 0xffff) - 32768) / 4;
        }

      vg_detector_stats_reset();
      vg_detector_set_dry_run(true);
      for (i = 0; i < n; i++)
        {
          vg_detector_process_window(pcm, VG_WINDOW_SAMPLES);
        }

      vg_detector_set_dry_run(false);
      vg_detector_stats(&st);
      printf("识别链路性能标定（%d 个 1 秒窗口，合成输入，不写事件日志）\n",
             n);
      printf("  单窗口平均 : %" PRIu32 " us\n", st.avg_us);
      printf("  单窗口峰值 : %" PRIu32 " us\n", st.max_us);
      printf("  实时率     : %.1f%% （占 1 秒窗口的比例，越低越省电）\n",
             (double)st.avg_us / 10000.0);
      printf("  预算       : < 500000 us（PRD-01）—— %s\n",
             st.max_us < 500000 ? "通过" : "超标");
      printf("  模型       : %s\n", vg_classifier_info());
      return st.max_us < 500000 ? 0 : 1;
    }

  if (strcmp(cmd, "selftest") == 0)
    {
      return vg_cmd_selftest();
    }

  printf("未知子命令: %s\n\n", cmd);
  vg_usage();
  return 1;
}

/****************************************************************************
 * 内置验收自检（PRD-08 可自动化部分，不依赖任何硬件）
 ****************************************************************************/

static int g_pass;
static int g_fail;

static void vg_check(const char *name, bool ok, const char *detail)
{
  if (ok)
    {
      g_pass++;
      printf("  [通过] %s\n", name);
    }
  else
    {
      g_fail++;
      printf("  [失败] %s —— %s\n", name, detail != NULL ? detail : "");
    }
}

static void vg_advance(uint64_t ms)
{
  uint64_t step = 200;
  uint64_t i;

  for (i = 0; i < ms; i += step)
    {
      vg_time_virtual_advance(step);
      vg_sm_tick();
    }
}

static int vg_cmd_selftest(void)
{
  vg_config_t *cfg = vg_config();
  vg_track_view_t view;
  uint32_t before;
  bool was_virtual = vg_time_is_virtual();

  g_pass = 0;
  g_fail = 0;

  printf("==== 安聆 VelaGuard 内置验收自检 ====\n");
  printf("（使用虚拟时钟，不依赖硬件与真实等待）\n\n");

  vg_time_use_virtual(true);
  vg_sm_reset();

  /* 场景 1：报警蜂鸣连续命中 -> 直接紧急级 -> 触发远程通知 */

  printf("场景1 烟雾报警声 -> 紧急级 -> 远程通知\n");
  before = vg_uploader_pending();
  vg_sm_inject(VG_EVT_ALARM_BEEP, 0.95f, "", "", 0.0f, 0);
  vg_advance(1000);
  vg_sm_inject(VG_EVT_ALARM_BEEP, 0.95f, "", "", 0.0f, 0);
  vg_advance(1000);
  vg_sm_inject(VG_EVT_ALARM_BEEP, 0.95f, "", "", 0.0f, 0);
  vg_advance(1000);
  vg_sm_inject(VG_EVT_ALARM_BEEP, 0.95f, "", "", 0.0f, 0);
  vg_advance(1000);
  vg_sm_inject(VG_EVT_ALARM_BEEP, 0.95f, "", "", 0.0f, 0);
  vg_advance(1500);
  vg_sm_inject(VG_EVT_ALARM_BEEP, 0.95f, "", "", 0.0f, 0);
  vg_sm_tick();

  vg_check("报警蜂鸣进入紧急级",
           vg_sm_top(&view) == 0 && view.evt.level == VG_LEVEL_EMERGENCY,
           "未达到紧急级");
  vg_check("紧急事件产生远程通知",
           vg_uploader_pending() > (int)before ||
           vg_uploader_online(),
           "通知未入队");
  vg_sm_ack(NULL, VG_STATUS_HANDLED);

  /* 场景 2：水流分级升级 */

  printf("场景2 持续水流 -> 提醒 -> 警告 -> 无人确认 -> 紧急\n");
  vg_sm_reset();
  vg_advance(cfg->event_idle_timeout_sec * 1000 + 1000);

  {
    uint32_t t;
    bool saw_notice = false;

    for (t = 0; t < cfg->water_notice_sec + 2; t++)
      {
        vg_sm_inject(VG_EVT_WATER_FLOW, 0.85f, "", "", 0.0f, 0);
        vg_advance(1000);
        if (vg_sm_top(&view) == 0 && view.evt.level == VG_LEVEL_NOTICE)
          {
            saw_notice = true;
          }
      }

    vg_check("水流先进入提醒级", saw_notice, "未出现提醒级");

    for (t = 0; t < cfg->water_warning_sec; t++)
      {
        vg_sm_inject(VG_EVT_WATER_FLOW, 0.85f, "", "", 0.0f, 0);
        vg_advance(1000);
        if (vg_sm_top(&view) == 0 && view.evt.level == VG_LEVEL_WARNING)
          {
            break;
          }
      }

    vg_check("水流升级为警告级",
             vg_sm_top(&view) == 0 && view.evt.level == VG_LEVEL_WARNING,
             "未升级到警告级");

    /* 倒计时无人确认 -> 紧急 */

    vg_advance((uint64_t)cfg->warning_countdown_sec * 1000 + 2000);
    vg_check("警告超时无确认升级为紧急",
             vg_sm_top(&view) == 0 && view.evt.level == VG_LEVEL_EMERGENCY,
             "超时未升级");

    vg_sm_ack(NULL, VG_STATUS_HANDLED);
    vg_check("确认已处理后事件关闭", vg_sm_active_count() == 0,
             "事件未关闭");
  }

  /* 场景 3：平静单次呼喊姓名不远程通知 */

  printf("场景3 平静单次呼喊姓名 -> 仅本地确认\n");
  vg_sm_reset();
  vg_advance(cfg->event_idle_timeout_sec * 1000 + 1000);
  before = (uint32_t)vg_uploader_pending();
  vg_sm_inject(VG_EVT_NAME_CALL_HELP, 0.9f, "老伴", "家庭成员 A", 0.2f, 0);
  vg_sm_tick();
  vg_check("平静呼喊只到提醒级",
           vg_sm_top(&view) == 0 && view.evt.level == VG_LEVEL_NOTICE &&
           view.local_only,
           "等级过高或未标记为仅本地");

  vg_advance((uint64_t)cfg->warning_countdown_sec * 1000 + 2000);
  vg_check("平静呼喊不产生远程通知",
           (uint32_t)vg_uploader_pending() == before,
           "产生了不该有的远程通知");

  /* 场景 4：急促重复呼喊升级 */

  printf("场景4 急促重复呼喊 -> 警告 -> 紧急\n");
  vg_sm_reset();
  vg_advance(cfg->event_idle_timeout_sec * 1000 + 1000);
  vg_sm_inject(VG_EVT_NAME_CALL_HELP, 0.9f, "救命", "家庭成员 A", 0.9f, 0);
  vg_advance(500);
  vg_sm_inject(VG_EVT_NAME_CALL_HELP, 0.9f, "救命", "家庭成员 A", 0.9f, 0);
  vg_sm_tick();
  vg_check("急促重复呼喊进入警告级",
           vg_sm_top(&view) == 0 && view.evt.level >= VG_LEVEL_WARNING,
           "未升级");

  vg_advance(500);
  vg_sm_inject(VG_EVT_NAME_CALL_HELP, 0.9f, "救命", "家庭成员 A", 0.9f, 0);
  vg_sm_tick();
  vg_check("持续呼救升级为紧急级",
           vg_sm_top(&view) == 0 && view.evt.level == VG_LEVEL_EMERGENCY,
           "未升级到紧急级");
  vg_sm_ack(NULL, VG_STATUS_HANDLED);

  /* 场景 5：snooze 两次后第三次超时升级 */

  printf("场景5 稍后提醒两次 -> 第三次超时升级\n");
  vg_sm_reset();
  vg_advance(cfg->event_idle_timeout_sec * 1000 + 1000);
  vg_sm_inject(VG_EVT_IMPACT, 0.9f, "", "", 0.0f, 0);
  vg_sm_tick();

  {
    bool ok = true;
    uint32_t k;

    for (k = 0; k < cfg->snooze_max_count; k++)
      {
        if (vg_sm_ack(NULL, VG_STATUS_SNOOZED) != 0)
          {
            ok = false;
          }

        vg_advance((uint64_t)cfg->snooze_minutes * 60 * 1000 + 1000);
      }

    vg_check("snooze 两次均被接受", ok, "snooze 被拒绝");
    vg_check("第三次 snooze 被拒绝",
             vg_sm_ack(NULL, VG_STATUS_SNOOZED) == -2,
             "第三次仍被接受");

    vg_advance((uint64_t)cfg->warning_countdown_sec * 1000 + 2000);
    vg_check("第三次超时按无人响应升级",
             vg_sm_top(&view) == 0 && view.evt.level == VG_LEVEL_EMERGENCY,
             "未升级");
    vg_sm_ack(NULL, VG_STATUS_FALSE_ALARM);
  }

  /* 场景 6：并发仲裁 —— 呼救优先于水流 */

  printf("场景6 并发注入水流 + 呼救 -> 呼救优先\n");
  vg_sm_reset();
  vg_advance(cfg->event_idle_timeout_sec * 1000 + 1000);

  {
    uint32_t t;

    for (t = 0; t <= cfg->water_notice_sec; t++)
      {
        vg_sm_inject(VG_EVT_WATER_FLOW, 0.85f, "", "", 0.0f, 0);
        vg_advance(1000);
      }

    vg_sm_inject(VG_EVT_DISTRESS_VOICE, 0.9f, "", "", 0.8f, 0);
    vg_sm_tick();

    vg_check("呼救事件优先展示",
             vg_sm_top(&view) == 0 &&
             view.evt.type == VG_EVT_DISTRESS_VOICE,
             "展示的不是呼救事件");
    vg_check("两个事件独立并存", vg_sm_active_count() == 2,
             "事件被合并或丢弃");
  }

  /* 场景 7：误报确认后不再升级 */

  printf("场景7 误报确认 -> 不再升级\n");
  vg_sm_ack(NULL, VG_STATUS_FALSE_ALARM);
  vg_advance((uint64_t)cfg->warning_countdown_sec * 1000 + 2000);
  vg_check("误报后不再产生紧急升级",
           vg_sm_top(&view) != 0 || view.evt.level != VG_LEVEL_EMERGENCY ||
           view.evt.type != VG_EVT_DISTRESS_VOICE,
           "误报事件仍被升级");

  /* 场景 8：时间不可信时夜间加严规则降级 */

  printf("场景8 时间未同步 -> 夜间加严规则降级\n");
  vg_sm_reset();
  vg_advance(cfg->event_idle_timeout_sec * 1000 + 1000);
  vg_time_set_reliable(false);
  vg_check("时间不可信时判定为日间", !vg_is_night(), vg_night_reason());

  vg_sm_inject(VG_EVT_IMPACT, 0.9f, "", "", 0.0f, 0);
  vg_sm_tick();
  vg_check("单次撞击只进入警告级（未按夜间加严）",
           vg_sm_top(&view) == 0 && view.evt.level == VG_LEVEL_WARNING,
           "被错误升级为紧急级");
  vg_sm_ack(NULL, VG_STATUS_HANDLED);

  /* 场景 9：事件日志环形覆盖 */

  printf("场景9 事件日志写满 100 条后环形覆盖\n");

  {
    vg_safety_event_t evt;
    int i;
    int n0 = vg_event_log_count();

    memset(&evt, 0, sizeof(evt));
    vg_strlcpy(evt.device_id, cfg->device_id, sizeof(evt.device_id));
    evt.type = VG_EVT_WATER_FLOW;
    evt.level = VG_LEVEL_NOTICE;
    evt.confidence = 0.5f;
    evt.started_at = vg_wall_sec();
    snprintf(evt.summary, sizeof(evt.summary), "日志容量测试");

    for (i = 0; i < VG_EVENT_LOG_CAPACITY + 5; i++)
      {
        snprintf(evt.event_id, sizeof(evt.event_id), "cap_%03d", i);
        vg_event_log_append(&evt);
      }

    vg_check("日志条数不超过 100",
             vg_event_log_count() == VG_EVENT_LOG_CAPACITY, "条数越界");
    vg_check("最旧记录已被覆盖",
             vg_event_log_find("cap_000", &evt) < 0, "旧记录未被覆盖");
    (void)n0;
  }

  /* 场景 10：LLM/Agent 不可用时模板文案兜底 */

  printf("场景10 Agent/LLM 不可用 -> 模板文案兜底\n");

  {
    vg_safety_event_t evt;
    char advice[VG_ADVICE_LEN];

    memset(&evt, 0, sizeof(evt));
    snprintf(evt.event_id, sizeof(evt.event_id), "evt_fallback");
    vg_strlcpy(evt.device_id, cfg->device_id, sizeof(evt.device_id));
    evt.type = VG_EVT_DISTRESS_VOICE;
    evt.level = VG_LEVEL_EMERGENCY;
    evt.confidence = 0.9f;
    evt.started_at = vg_wall_sec();
    snprintf(evt.summary, sizeof(evt.summary), "检测到疑似求助呼喊");

    vg_notifier_build_advice(&evt, advice, sizeof(advice));
    vg_check("通知文案非空（Agent 失败也不丢通知）",
             advice[0] != '\0', "文案为空");
    printf("       文案: %s\n", advice);
  }

  /* 协议校验 */

  printf("场景11 结构化事件协议序列化与校验\n");

  {
    vg_safety_event_t a;
    vg_safety_event_t b;
    char json[768];
    vg_event_envelope_t envelope;
    char envelope_json[2048];

    memset(&a, 0, sizeof(a));
    snprintf(a.event_id, sizeof(a.event_id), "evt_001");
    vg_strlcpy(a.device_id, "velaguard_demo_001", sizeof(a.device_id));
    a.type = VG_EVT_NAME_CALL_HELP;
    a.level = VG_LEVEL_WARNING;
    a.confidence = 0.86f;
    a.started_at = 1780000000;
    a.duration_sec = 8;
    a.local_status = VG_STATUS_NO_RESPONSE;
    a.upload_reason = VG_UPLOAD_ESCALATED;
    a.repeat_count = 3;
    a.urgency = 0.9f;
    a.time_reliable = true;
    vg_strlcpy(a.matched_phrase, "快来人", sizeof(a.matched_phrase));
    vg_strlcpy(a.person_label, "家庭成员 A", sizeof(a.person_label));
    snprintf(a.summary, sizeof(a.summary), "夜间检测到急促求助呼喊，用户未确认");

    vg_check("事件字段校验通过", vg_event_validate(&a) == 0, "字段非法");
    vg_check("事件可序列化为 JSON",
             vg_event_to_json(&a, json, sizeof(json)) > 0, "序列化失败");
    vg_check("JSON 可解析回结构体",
             vg_event_from_json(json, &b) == 0 &&
             b.type == a.type && b.level == a.level &&
             b.started_at == a.started_at &&
             strcmp(b.matched_phrase, a.matched_phrase) == 0,
             "解析结果不一致");
    printf("       JSON: %s\n", json);

    vg_check("event.v1 envelope 含 advice 且可回读",
             vg_event_envelope_to_json(&a, "请电话确认现场情况",
                                       "msg_selftest_1", "b0001abcd", 1,
                                       1, "trace_selftest_1", 1234,
                                       envelope_json, sizeof(envelope_json)) > 0 &&
             strstr(envelope_json, "\"advice\":") != NULL &&
             vg_event_envelope_from_json(envelope_json, &envelope) == 0 &&
             strcmp(envelope.advice, "请电话确认现场情况") == 0,
             "envelope 序列化或解析失败");
  }

  vg_sm_reset();
  vg_time_use_virtual(was_virtual);

  printf("\n==== 自检结果: 通过 %d 项，失败 %d 项 ====\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
