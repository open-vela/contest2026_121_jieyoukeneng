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

/* v2 帧级统计缓存：由 vg_feature_mfcc 在同一遍 FFT 中填充，
 * vg_feature_extract 紧随其后聚合成 13 维描述子
 */

static float g_frame_edb[VG_MAX_FRAMES];    /* 帧能量 (dB) */
static float g_frame_cent[VG_MAX_FRAMES];   /* 归一化谱质心 */
static float g_frame_hf1k[VG_MAX_FRAMES];   /* >1kHz 能量占比 */
static float g_frame_hf4k[VG_MAX_FRAMES];   /* >4kHz 能量占比 */

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

      /* 帧级功率谱统计（v2）：能量、谱质心、高频占比。
       * 与 model/velaguard_features.py _frames() 逐步骤一致。
       */

        {
          float p_sum = 0.0f;
          float p_wsum = 0.0f;
          float p_hf1k = 0.0f;
          float p_hf4k = 0.0f;

          for (j = 0; j <= VG_FFT_SIZE / 2; j++)
            {
              float p = re[j] * re[j] + im[j] * im[j];

              p_sum += p;
              p_wsum += (float)j * p;
              if (j >= VG_BIN_1K)
                {
                  p_hf1k += p;
                }

              if (j >= VG_BIN_4K)
                {
                  p_hf4k += p;
                }
            }

          g_frame_edb[nframes] = 10.0f * log10f(p_sum + VG_EPS);
          g_frame_cent[nframes] = p_wsum / (p_sum + VG_EPS) / VG_NYQ_BIN;
          g_frame_hf1k[nframes] = p_hf1k / (p_sum + VG_EPS);
          g_frame_hf4k[nframes] = p_hf4k / (p_sum + VG_EPS);
        }

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

  feat[VG_FEAT_ZCR] = (nsamples > 1)
                      ? (float)zc / (float)(nsamples - 1) : 0.0f;

  /* ---- v2 追加维度（40-52），与 extract_v2() 逐维一致 ---- */

    {
      float e_mean = 0.0f;
      float e_max;
      float max_rise = 0.0f;
      float max_fall = 0.0f;
      float c_sum = 0.0f;
      float c_sum2 = 0.0f;
      float c_dsum = 0.0f;
      float hf1k_sum = 0.0f;
      float hf4k_sum = 0.0f;
      float var;
      int   nhigh = 0;

      e_max = g_frame_edb[0];
      for (f = 0; f < nframes; f++)
        {
          e_mean += g_frame_edb[f];
          if (g_frame_edb[f] > e_max)
            {
              e_max = g_frame_edb[f];
            }

          if (f > 0)
            {
              float d = g_frame_edb[f] - g_frame_edb[f - 1];

              if (d > max_rise)
                {
                  max_rise = d;
                }

              if (-d > max_fall)
                {
                  max_fall = -d;
                }

              c_dsum += fabsf(g_frame_cent[f] - g_frame_cent[f - 1]);
            }

          c_sum += g_frame_cent[f];
          c_sum2 += g_frame_cent[f] * g_frame_cent[f];
          hf1k_sum += g_frame_hf1k[f];
          hf4k_sum += g_frame_hf4k[f];
        }

      e_mean /= nframes;

      for (f = 0; f < nframes; f++)
        {
          if (g_frame_edb[f] > e_mean)
            {
              nhigh++;
            }
        }

      feat[VG_FEAT_CREST] = e_max - e_mean;
      feat[41] = (nframes > 1) ? max_rise : 0.0f;
      feat[42] = (nframes > 1) ? max_fall : 0.0f;
      feat[43] = (float)nhigh / (float)nframes;

      /* 4 段能量轮廓（相对全窗均值）；分段规则与 np.array_split 一致：
       * 前 rem 段各 base+1 帧，其余 base 帧。帧数不足 4 时置 0。
       */

      if (nframes >= 4)
        {
          int base = nframes / 4;
          int rem = nframes % 4;
          int pos = 0;

          for (i = 0; i < 4; i++)
            {
              int len = base + (i < rem ? 1 : 0);
              float acc = 0.0f;

              for (f = 0; f < len; f++)
                {
                  acc += g_frame_edb[pos + f];
                }

              feat[44 + i] = acc / len - e_mean;
              pos += len;
            }
        }
      else
        {
          feat[44] = feat[45] = feat[46] = feat[47] = 0.0f;
        }

      feat[VG_FEAT_CENT_MEAN] = c_sum / nframes;
      var = c_sum2 / nframes -
            (c_sum / nframes) * (c_sum / nframes);
      feat[49] = (var > 0.0f) ? sqrtf(var) : 0.0f;
      feat[50] = (nframes > 1) ? c_dsum / (nframes - 1) : 0.0f;
      feat[51] = hf1k_sum / nframes;
      feat[VG_FEAT_HF4K] = hf4k_sum / nframes;
    }

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
