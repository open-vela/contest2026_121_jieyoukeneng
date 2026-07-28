/****************************************************************************
 * 安聆 VelaGuard - 端侧分类器实现 (PRD-01/07)
 *
 * 两层 MLP，权重 int8 量化 + per-tensor scale 反量化，纯 C 实现，
 * 不引入任何第三方推理运行时。
 ****************************************************************************/

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "velaguard/vg_classifier.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint32_t g_infer_count;
static uint64_t g_infer_total_us;
static uint32_t g_infer_max_us;
static char     g_info[128];

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

static void vg_softmax(float *v, int n)
{
  float max = v[0];
  float sum = 0.0f;
  int i;

  for (i = 1; i < n; i++)
    {
      if (v[i] > max)
        {
          max = v[i];
        }
    }

  for (i = 0; i < n; i++)
    {
      v[i] = expf(v[i] - max);
      sum += v[i];
    }

  if (sum <= 0.0f)
    {
      sum = 1.0f;
    }

  for (i = 0; i < n; i++)
    {
      v[i] /= sum;
    }
}

/* 通用两层 MLP 前向 */

static int vg_mlp_forward(const float *feat,
                          const float *mean, const float *scale,
                          const signed char *w1, float w1_scale,
                          const float *b1, int hidden,
                          const signed char *w2, float w2_scale,
                          const float *b2, int classes,
                          float *probs)
{
  float norm[VG_FEATURE_DIM];
  float h[VG_ENV_HIDDEN > VG_VOICE_HIDDEN ?
          VG_ENV_HIDDEN : VG_VOICE_HIDDEN];
  float out[VG_ENV_CLASSES > VG_VOICE_CLASSES ?
            VG_ENV_CLASSES : VG_VOICE_CLASSES];
  uint64_t t0 = vg_micros();
  uint64_t dt;
  int best = 0;
  int i;
  int j;

  for (i = 0; i < VG_FEATURE_DIM; i++)
    {
      float s = (scale[i] > 1e-6f) ? scale[i] : 1.0f;

      norm[i] = (feat[i] - mean[i]) / s;
    }

  for (i = 0; i < hidden; i++)
    {
      float acc = 0.0f;

      for (j = 0; j < VG_FEATURE_DIM; j++)
        {
          acc += (float)w1[i * VG_FEATURE_DIM + j] * norm[j];
        }

      acc = acc * w1_scale + b1[i];
      h[i] = (acc > 0.0f) ? acc : 0.0f;   /* ReLU */
    }

  for (i = 0; i < classes; i++)
    {
      float acc = 0.0f;

      for (j = 0; j < hidden; j++)
        {
          acc += (float)w2[i * hidden + j] * h[j];
        }

      out[i] = acc * w2_scale + b2[i];
    }

  vg_softmax(out, classes);

  for (i = 1; i < classes; i++)
    {
      if (out[i] > out[best])
        {
          best = i;
        }
    }

  if (probs != NULL)
    {
      memcpy(probs, out, sizeof(float) * (size_t)classes);
    }

  dt = vg_micros() - t0;
  g_infer_count++;
  g_infer_total_us += dt;
  if ((uint32_t)dt > g_infer_max_us)
    {
      g_infer_max_us = (uint32_t)dt;
    }

  return best;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

vg_sound_class_t vg_classify_env(const float *feat, float *probs)
{
  int best;

  if (feat == NULL)
    {
      return VG_SOUND_OTHER;
    }

  best = vg_mlp_forward(feat, g_vg_env_mean, g_vg_env_scale,
                        &g_vg_env_w1[0][0], g_vg_env_w1_scale, g_vg_env_b1,
                        VG_ENV_HIDDEN,
                        &g_vg_env_w2[0][0], g_vg_env_w2_scale, g_vg_env_b2,
                        VG_ENV_CLASSES, probs);

  return (vg_sound_class_t)best;
}

vg_voice_kind_t vg_classify_voice(const float *feat, float *probs)
{
  static const vg_voice_kind_t map[VG_VOICE_CLASSES] =
    {
      VG_VOICE_NONE, VG_VOICE_MOAN, VG_VOICE_SCREAM, VG_VOICE_SHOUT_HELP
    };
  int best;

  if (feat == NULL)
    {
      return VG_VOICE_NONE;
    }

  best = vg_mlp_forward(feat, g_vg_voice_mean, g_vg_voice_scale,
                        &g_vg_voice_w1[0][0], g_vg_voice_w1_scale,
                        g_vg_voice_b1, VG_VOICE_HIDDEN,
                        &g_vg_voice_w2[0][0], g_vg_voice_w2_scale,
                        g_vg_voice_b2, VG_VOICE_CLASSES, probs);

  return map[best];
}

const char *vg_classifier_info(void)
{
  size_t env_bytes = sizeof(g_vg_env_w1) + sizeof(g_vg_env_w2) +
                     sizeof(g_vg_env_b1) + sizeof(g_vg_env_b2) +
                     sizeof(g_vg_env_mean) + sizeof(g_vg_env_scale);
  size_t voice_bytes = sizeof(g_vg_voice_w1) + sizeof(g_vg_voice_w2) +
                       sizeof(g_vg_voice_b1) + sizeof(g_vg_voice_b2) +
                       sizeof(g_vg_voice_mean) + sizeof(g_vg_voice_scale);

  snprintf(g_info, sizeof(g_info),
           "MFCC(%d)+MLP int8 | 版本 %s | 训练集 %s | 权重 %zu 字节",
           VG_FEATURE_DIM, g_vg_model_version, g_vg_model_trained_on,
           env_bytes + voice_bytes);

  return g_info;
}

void vg_classifier_perf(uint32_t *count, uint32_t *avg_us, uint32_t *max_us)
{
  if (count != NULL)
    {
      *count = g_infer_count;
    }

  if (avg_us != NULL)
    {
      *avg_us = (g_infer_count > 0)
                ? (uint32_t)(g_infer_total_us / g_infer_count) : 0;
    }

  if (max_us != NULL)
    {
      *max_us = g_infer_max_us;
    }
}

void vg_classifier_perf_reset(void)
{
  g_infer_count = 0;
  g_infer_total_us = 0;
  g_infer_max_us = 0;
}
