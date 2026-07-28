/****************************************************************************
 * 安聆 VelaGuard - 声学识别引擎调度实现 (PRD-01)
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "velaguard/vg_classifier.h"
#include "velaguard/vg_detector.h"
#include "velaguard/vg_enroll.h"
#include "velaguard/vg_event_sm.h"
#include "velaguard/vg_feature.h"
#include "velaguard/vg_time.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int16_t             g_window[VG_WINDOW_SAMPLES];
static size_t              g_filled;
static bool                g_window_ready;
static vg_detector_stats_t g_stats;
static uint64_t            g_total_us;
static float               g_silence_db = -55.0f;
static bool                g_dry_run;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t vg_micros(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* 急促程度估计：类别基线 + 能量修正，范围 0~1 */

static float vg_estimate_urgency(vg_voice_kind_t kind, float energy_db,
                                 float zcr)
{
  float base;

  switch (kind)
    {
      case VG_VOICE_SCREAM:
        base = 0.85f;
        break;

      case VG_VOICE_SHOUT_HELP:
        base = 0.75f;
        break;

      case VG_VOICE_MOAN:
        base = 0.30f;
        break;

      default:
        base = 0.20f;
        break;
    }

  /* 越响、过零率越高，通常越急促 */

  base += (energy_db + 40.0f) / 200.0f;
  base += zcr * 0.2f;

  if (base < 0.0f)
    {
      base = 0.0f;
    }

  if (base > 1.0f)
    {
      base = 1.0f;
    }

  return base;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_detector_init(void)
{
  memset(g_window, 0, sizeof(g_window));
  g_filled = 0;
  g_window_ready = false;
  vg_detector_stats_reset();
  return 0;
}

void vg_detector_deinit(void)
{
  g_filled = 0;
  g_window_ready = false;
}

void vg_detector_set_silence_db(float db)
{
  g_silence_db = db;
}

void vg_detector_set_dry_run(bool on)
{
  g_dry_run = on;
}

int vg_detector_process_window(const int16_t *pcm, size_t nsamples)
{
  float feat[VG_FEATURE_DIM];
  float env_probs[VG_ENV_CLASSES];
  float voice_probs[VG_VOICE_CLASSES];
  vg_enroll_match_t match;
  vg_sound_class_t cls;
  vg_voice_kind_t kind;
  uint64_t t0 = vg_micros();
  uint64_t dt;
  float energy;
  uint64_t now;

  if (pcm == NULL || nsamples < VG_FRAME_LEN)
    {
      return -1;
    }

  now = vg_now_ms();
  energy = vg_feature_energy_db(pcm, nsamples);
  g_stats.last_energy_db = energy;
  g_stats.windows++;

  if (energy < g_silence_db)
    {
      /* 静音窗口：不推理，直接返回 */

      g_stats.last_sound = VG_SOUND_OTHER;
      g_stats.last_voice = VG_VOICE_NONE;
      return 0;
    }

  if (vg_feature_extract(pcm, nsamples, feat) < 0)
    {
      return -1;
    }

  /* 通道 1：环境声景 */

  cls = vg_classify_env(feat, env_probs);
  g_stats.last_sound = cls;
  g_stats.last_sound_conf = env_probs[cls];

  if (cls != VG_SOUND_OTHER)
    {
      vg_sound_obs_t obs;

      memset(&obs, 0, sizeof(obs));
      obs.ts_ms = now;
      obs.cls = cls;
      obs.confidence = env_probs[cls];
      obs.window_ms = (uint16_t)(nsamples * 1000 / VG_SAMPLE_RATE);

      g_stats.sound_hits++;
      if (!g_dry_run)
        {
          vg_sm_feed_sound(&obs);
        }
    }

  /* 通道 2：人声异常 */

  kind = vg_classify_voice(feat, voice_probs);
  g_stats.last_voice = kind;
  g_stats.last_voice_conf = voice_probs[kind == VG_VOICE_NONE ? 0 :
                                        (kind == VG_VOICE_MOAN ? 1 :
                                         (kind == VG_VOICE_SCREAM ? 2 : 3))];

  if (kind != VG_VOICE_NONE)
    {
      vg_distress_obs_t obs;

      memset(&obs, 0, sizeof(obs));
      obs.ts_ms = now;
      obs.kind = kind;
      obs.confidence = g_stats.last_voice_conf;
      obs.urgency = vg_estimate_urgency(kind, energy,
                                        feat[VG_FEATURE_DIM - 1]);
      obs.repeat_count = 1;

      g_stats.voice_hits++;
      if (!g_dry_run)
        {
          vg_sm_feed_distress(&obs);
        }
    }

  /* 通道 3：个性化模板（姓名 / 称呼 / 求救词 / 方言） */

  if (vg_enroll_match(feat, &match) == 0)
    {
      vg_distress_obs_t obs;

      memset(&obs, 0, sizeof(obs));
      obs.ts_ms = now;
      obs.kind = (match.kind == VG_TPL_NAME) ?
                 VG_VOICE_NAME_CALL : VG_VOICE_HELP_PHRASE;
      obs.confidence = match.score;
      obs.repeat_count = 1;

      /* 模板本身区分平静/急促，比能量估计更可靠 */

      obs.urgency = (match.style == VG_TPL_URGENT) ? 0.85f : 0.20f;
      if (kind == VG_VOICE_SCREAM || kind == VG_VOICE_SHOUT_HELP)
        {
          obs.urgency = 0.95f;
        }

      vg_strlcpy(obs.matched_phrase, match.phrase, sizeof(obs.matched_phrase));
      vg_strlcpy(obs.person_label, match.person, sizeof(obs.person_label));

      g_stats.template_hits++;
      if (!g_dry_run)
        {
          vg_sm_feed_distress(&obs);
        }
    }

  dt = vg_micros() - t0;
  g_total_us += dt;
  if ((uint32_t)dt > g_stats.max_us)
    {
      g_stats.max_us = (uint32_t)dt;
    }

  g_stats.avg_us = (uint32_t)(g_total_us / g_stats.windows);
  return 1;
}

int vg_detector_pump(void)
{
  int n;

  if (vg_capture_source() == VG_SRC_NONE)
    {
      return 0;
    }

  if (!g_window_ready)
    {
      /* 首个窗口：填满 1 秒 */

      n = vg_capture_read(g_window + g_filled, VG_WINDOW_SAMPLES - g_filled);
      if (n <= 0)
        {
          return 0;
        }

      g_filled += (size_t)n;
      if (g_filled < VG_WINDOW_SAMPLES)
        {
          return 0;
        }

      g_window_ready = true;
      return vg_detector_process_window(g_window, VG_WINDOW_SAMPLES) > 0
             ? 1 : 0;
    }

  /* 后续窗口：50% 重叠，滑入半秒新数据 */

  memmove(g_window, g_window + VG_HOP_SAMPLES,
          (VG_WINDOW_SAMPLES - VG_HOP_SAMPLES) * sizeof(int16_t));

  n = vg_capture_read(g_window + (VG_WINDOW_SAMPLES - VG_HOP_SAMPLES),
                      VG_HOP_SAMPLES);
  if (n <= 0)
    {
      /* 数据不足：把窗口移回原位，等待下次 */

      memmove(g_window + VG_HOP_SAMPLES, g_window,
              (VG_WINDOW_SAMPLES - VG_HOP_SAMPLES) * sizeof(int16_t));
      return 0;
    }

  if ((size_t)n < VG_HOP_SAMPLES)
    {
      memset(g_window + (VG_WINDOW_SAMPLES - VG_HOP_SAMPLES) + n, 0,
             (VG_HOP_SAMPLES - (size_t)n) * sizeof(int16_t));
    }

  return vg_detector_process_window(g_window, VG_WINDOW_SAMPLES) > 0 ? 1 : 0;
}

int vg_detector_run_file(const char *path)
{
  bool was_virtual = vg_time_is_virtual();
  int windows = 0;

  if (vg_capture_open(VG_SRC_WAV, path) < 0)
    {
      return -1;
    }

  g_filled = 0;
  g_window_ready = false;

  /* wav 回放远快于实时，必须用虚拟时钟按窗口节奏推进，
   * 否则持续时间类规则（水流 30s/120s、报警连续 5s）永远无法触发。
   */

  vg_time_use_virtual(true);

  while (1)
    {
      bool first = !g_window_ready;

      if (vg_detector_pump() <= 0)
        {
          break;
        }

      windows++;

      /* 首窗覆盖 1 秒，其后每窗推进 0.5 秒（50% 重叠） */

      vg_time_virtual_advance(first ? 1000 : VG_HOP_SAMPLES * 1000 /
                                             VG_SAMPLE_RATE);
      vg_sm_tick();
    }

  vg_capture_close();
  vg_time_use_virtual(was_virtual);
  return windows;
}

void vg_detector_stats(vg_detector_stats_t *out)
{
  if (out != NULL)
    {
      *out = g_stats;
    }
}

void vg_detector_stats_reset(void)
{
  memset(&g_stats, 0, sizeof(g_stats));
  g_total_us = 0;
  vg_classifier_perf_reset();
}
