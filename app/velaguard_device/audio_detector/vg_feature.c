/****************************************************************************
 * 安聆 VelaGuard - 声学特征提取实现 (PRD-01/07)
 *
 * 与 model/velaguard_features.py 保持逐步骤一致，保证电脑端训练与端侧推理
 * 使用同一套特征定义。
 ****************************************************************************/

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "velaguard/vg_feature.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_PREEMPH   0.97f
#define VG_MEL_LOW   20.0f
#define VG_MEL_HIGH  7800.0f
#define VG_EPS       1e-10f

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static float g_hamming[VG_FRAME_LEN];
static float g_mel_fb[VG_NUM_MEL][VG_FFT_SIZE / 2 + 1];
static float g_dct[VG_NUM_MFCC][VG_NUM_MEL];
static int   g_fft_rev[VG_FFT_SIZE];
static float g_cos_tab[VG_FFT_SIZE / 2];
static float g_sin_tab[VG_FFT_SIZE / 2];
static int   g_tables_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static float vg_hz2mel(float hz)
{
  return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static float vg_mel2hz(float mel)
{
  return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

static void vg_build_tables(void)
{
  float mel_low;
  float mel_high;
  float points[VG_NUM_MEL + 2];
  int bins[VG_NUM_MEL + 2];
  int i;
  int j;
  int bits = 0;
  int n = VG_FFT_SIZE;

  if (g_tables_ready)
    {
      return;
    }

  for (i = 0; i < VG_FRAME_LEN; i++)
    {
      g_hamming[i] = 0.54f - 0.46f *
                     cosf(2.0f * (float)M_PI * i / (VG_FRAME_LEN - 1));
    }

  /* FFT 位反转表与旋转因子 */

  while ((1 << bits) < n)
    {
      bits++;
    }

  for (i = 0; i < n; i++)
    {
      int r = 0;

      for (j = 0; j < bits; j++)
        {
          r |= ((i >> j) & 1) << (bits - 1 - j);
        }

      g_fft_rev[i] = r;
    }

  for (i = 0; i < n / 2; i++)
    {
      g_cos_tab[i] = cosf(-2.0f * (float)M_PI * i / n);
      g_sin_tab[i] = sinf(-2.0f * (float)M_PI * i / n);
    }

  /* Mel 滤波器组 */

  mel_low = vg_hz2mel(VG_MEL_LOW);
  mel_high = vg_hz2mel(VG_MEL_HIGH);

  for (i = 0; i < VG_NUM_MEL + 2; i++)
    {
      points[i] = vg_mel2hz(mel_low +
                            (mel_high - mel_low) * i / (VG_NUM_MEL + 1));
      bins[i] = (int)floorf((VG_FFT_SIZE + 1) * points[i] / VG_SAMPLE_RATE);
      if (bins[i] > VG_FFT_SIZE / 2)
        {
          bins[i] = VG_FFT_SIZE / 2;
        }
    }

  memset(g_mel_fb, 0, sizeof(g_mel_fb));
  for (i = 0; i < VG_NUM_MEL; i++)
    {
      for (j = bins[i]; j < bins[i + 1]; j++)
        {
          if (bins[i + 1] > bins[i])
            {
              g_mel_fb[i][j] = (float)(j - bins[i]) /
                               (float)(bins[i + 1] - bins[i]);
            }
        }

      for (j = bins[i + 1]; j < bins[i + 2]; j++)
        {
          if (bins[i + 2] > bins[i + 1])
            {
              g_mel_fb[i][j] = (float)(bins[i + 2] - j) /
                               (float)(bins[i + 2] - bins[i + 1]);
            }
        }
    }

  /* DCT-II 正交化系数 */

  for (i = 0; i < VG_NUM_MFCC; i++)
    {
      for (j = 0; j < VG_NUM_MEL; j++)
        {
          g_dct[i][j] = cosf((float)M_PI * i * (2.0f * j + 1.0f) /
                             (2.0f * VG_NUM_MEL));
        }
    }

  g_tables_ready = 1;
}

/* 就地基 2 FFT，re/im 长度 VG_FFT_SIZE */

static void vg_fft(float *re, float *im)
{
  int n = VG_FFT_SIZE;
  int len;
  int i;

  for (i = 0; i < n; i++)
    {
      int r = g_fft_rev[i];

      if (r > i)
        {
          float t = re[i]; re[i] = re[r]; re[r] = t;
          t = im[i]; im[i] = im[r]; im[r] = t;
        }
    }

  for (len = 2; len <= n; len <<= 1)
    {
      int half = len >> 1;
      int step = n / len;

      for (i = 0; i < n; i += len)
        {
          int k;

          for (k = 0; k < half; k++)
            {
              float wr = g_cos_tab[k * step];
              float wi = g_sin_tab[k * step];
              float xr = re[i + k + half] * wr - im[i + k + half] * wi;
              float xi = re[i + k + half] * wi + im[i + k + half] * wr;

              re[i + k + half] = re[i + k] - xr;
              im[i + k + half] = im[i + k] - xi;
              re[i + k] += xr;
              im[i + k] += xi;
            }
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_feature_mfcc(const int16_t *pcm, size_t nsamples, float *mfcc)
{
  static float re[VG_FFT_SIZE];
  static float im[VG_FFT_SIZE];
  static float mel[VG_NUM_MEL];
  float prev = 0.0f;
  int nframes = 0;
  size_t off;

  if (pcm == NULL || mfcc == NULL || nsamples < VG_FRAME_LEN)
    {
      return -1;
    }

  vg_build_tables();

  for (off = 0;
       off + VG_FRAME_LEN <= nsamples && nframes < VG_MAX_FRAMES;
       off += VG_FRAME_HOP)
    {
      float *out = mfcc + (size_t)nframes * VG_NUM_MFCC;
      int i;
      int j;

      memset(re, 0, sizeof(re));
      memset(im, 0, sizeof(im));

      /* 预加重 + 加窗 */

      prev = (off > 0) ? (float)pcm[off - 1] : 0.0f;
      for (i = 0; i < VG_FRAME_LEN; i++)
        {
          float s = (float)pcm[off + i];

          re[i] = (s - VG_PREEMPH * prev) * g_hamming[i] / 32768.0f;
          prev = s;
        }

      vg_fft(re, im);

      /* 功率谱 -> mel 能量 */

      for (i = 0; i < VG_NUM_MEL; i++)
        {
          float acc = 0.0f;

          for (j = 0; j <= VG_FFT_SIZE / 2; j++)
            {
              float w = g_mel_fb[i][j];

              if (w > 0.0f)
                {
                  acc += w * (re[j] * re[j] + im[j] * im[j]);
                }
            }

          mel[i] = logf(acc + VG_EPS);
        }

      /* DCT -> MFCC */

      for (i = 0; i < VG_NUM_MFCC; i++)
        {
          float acc = 0.0f;

          for (j = 0; j < VG_NUM_MEL; j++)
            {
              acc += mel[j] * g_dct[i][j];
            }

          out[i] = acc / VG_NUM_MEL;
        }

      nframes++;
    }

  return nframes;
}

int vg_feature_extract(const int16_t *pcm, size_t nsamples, float *feat)
{
  static float mfcc[VG_MAX_FRAMES * VG_NUM_MFCC];
  int nframes;
  int i;
  int f;
  size_t k;
  uint32_t zc = 0;

  if (feat == NULL)
    {
      return -1;
    }

  nframes = vg_feature_mfcc(pcm, nsamples, mfcc);
  if (nframes <= 0)
    {
      return -1;
    }

  for (i = 0; i < VG_NUM_MFCC; i++)
    {
      float sum = 0.0f;
      float sum2 = 0.0f;
      float dsum = 0.0f;
      float mean;
      float var;

      for (f = 0; f < nframes; f++)
        {
          float v = mfcc[(size_t)f * VG_NUM_MFCC + i];

          sum += v;
          sum2 += v * v;

          if (f > 0)
            {
              dsum += fabsf(v - mfcc[(size_t)(f - 1) * VG_NUM_MFCC + i]);
            }
        }

      mean = sum / nframes;
      var = sum2 / nframes - mean * mean;
      if (var < 0.0f)
        {
          var = 0.0f;
        }

      feat[i] = mean;
      feat[VG_NUM_MFCC + i] = sqrtf(var);
      feat[VG_NUM_MFCC * 2 + i] =
          (nframes > 1) ? dsum / (nframes - 1) : 0.0f;
    }

  /* 过零率：对瞬态冲击与蜂鸣的区分很有帮助 */

  for (k = 1; k < nsamples; k++)
    {
      if ((pcm[k] >= 0) != (pcm[k - 1] >= 0))
        {
          zc++;
        }
    }

  feat[VG_FEATURE_DIM - 1] = (nsamples > 1)
                             ? (float)zc / (float)(nsamples - 1) : 0.0f;

  return VG_FEATURE_DIM;
}

float vg_feature_energy_db(const int16_t *pcm, size_t nsamples)
{
  double acc = 0.0;
  size_t i;

  if (pcm == NULL || nsamples == 0)
    {
      return -120.0f;
    }

  for (i = 0; i < nsamples; i++)
    {
      double v = (double)pcm[i] / 32768.0;

      acc += v * v;
    }

  acc = sqrt(acc / (double)nsamples);
  return (float)(20.0 * log10(acc + 1e-9));
}
