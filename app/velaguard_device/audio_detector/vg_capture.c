/****************************************************************************
 * 安聆 VelaGuard - 音频采集抽象实现 (PRD-01)
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "velaguard/vg_types.h"
#include "velaguard/vg_capture.h"
#include "velaguard/vg_feature.h"
#include "velaguard/vg_time.h"

#ifndef CONFIG_VELAGUARD_PCM_DEVICE
#  define CONFIG_VELAGUARD_PCM_DEVICE "/dev/audio/pcm0c"
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct
{
  uint16_t channels;
  uint32_t rate;
  uint16_t bits;
  long     data_off;
  long     data_len;
} vg_wav_info_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_source_t g_source = VG_SRC_NONE;
static FILE       *g_wav;
static vg_wav_info_t g_wav_info;
static long        g_wav_pos;
static int         g_mic_fd = -1;
static bool        g_paused;
static char        g_pause_reason[48];
static uint64_t    g_hold_until_ms;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t vg_rd32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t vg_rd16(const unsigned char *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int vg_wav_parse(FILE *fp, vg_wav_info_t *info)
{
  unsigned char hdr[12];
  unsigned char chunk[8];

  if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
      memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
    {
      return -1;
    }

  memset(info, 0, sizeof(*info));

  while (fread(chunk, 1, sizeof(chunk), fp) == sizeof(chunk))
    {
      uint32_t size = vg_rd32(chunk + 4);
      long next = ftell(fp) + (long)size + (size & 1);

      if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16)
        {
          unsigned char fmt[16];

          if (fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt))
            {
              return -1;
            }

          info->channels = vg_rd16(fmt + 2);
          info->rate = vg_rd32(fmt + 4);
          info->bits = vg_rd16(fmt + 14);
        }
      else if (memcmp(chunk, "data", 4) == 0)
        {
          info->data_off = ftell(fp);
          info->data_len = (long)size;
          break;
        }

      if (fseek(fp, next, SEEK_SET) != 0)
        {
          break;
        }
    }

  if (info->data_off == 0 || info->channels == 0 || info->rate == 0)
    {
      return -1;
    }

  if (info->bits != 16)
    {
      fprintf(stderr, "[velaguard] 仅支持 16bit wav（当前 %ubit），"
                      "请先用 model/prepare_dataset.py 转换\n",
              (unsigned)info->bits);
      return -1;
    }

  return 0;
}

/* 从 wav 读取并重采样为 16kHz 单声道 */

static int vg_wav_read(int16_t *buf, size_t nsamples)
{
  size_t produced = 0;
  int16_t frame[8];

  while (produced < nsamples)
    {
      /* 线性最近邻重采样：按目标索引换算源索引 */

      double ratio = (double)g_wav_info.rate / (double)VG_SAMPLE_RATE;
      long src_index = (long)((double)g_wav_pos * ratio);
      long byte_off = g_wav_info.data_off +
                      src_index * 2 * (long)g_wav_info.channels;
      size_t framebytes = 2 * (size_t)g_wav_info.channels;
      int32_t acc = 0;
      int c;

      if (byte_off + (long)framebytes >
          g_wav_info.data_off + g_wav_info.data_len)
        {
          break;
        }

      if (fseek(g_wav, byte_off, SEEK_SET) != 0 ||
          fread(frame, 1, framebytes, g_wav) != framebytes)
        {
          break;
        }

      for (c = 0; c < (int)g_wav_info.channels && c < 8; c++)
        {
          acc += frame[c];
        }

      buf[produced++] = (int16_t)(acc / (int)g_wav_info.channels);
      g_wav_pos++;
    }

  return (int)produced;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_capture_open(vg_source_t src, const char *path)
{
  vg_capture_close();

  switch (src)
    {
      case VG_SRC_WAV:
        if (path == NULL)
          {
            return -1;
          }

        g_wav = fopen(path, "rb");
        if (g_wav == NULL)
          {
            fprintf(stderr, "[velaguard] 打开 wav 失败: %s\n", path);
            return -1;
          }

        if (vg_wav_parse(g_wav, &g_wav_info) < 0)
          {
            fclose(g_wav);
            g_wav = NULL;
            return -1;
          }

        g_wav_pos = 0;
        g_source = VG_SRC_WAV;
        printf("[velaguard] 音频源: wav %s (%uHz %uch)\n",
               path, (unsigned)g_wav_info.rate,
               (unsigned)g_wav_info.channels);
        return 0;

      case VG_SRC_MIC:
        g_mic_fd = open(path != NULL ? path : CONFIG_VELAGUARD_PCM_DEVICE,
                        O_RDONLY);
        if (g_mic_fd < 0)
          {
            fprintf(stderr,
                    "[velaguard] 麦克风设备不可用(%s)。"
                    "模拟器线请使用 wav 音频源或模拟事件注入。\n",
                    strerror(errno));
            return -1;
          }

        g_source = VG_SRC_MIC;
        printf("[velaguard] 音频源: 板载麦克风 %s\n",
               path != NULL ? path : CONFIG_VELAGUARD_PCM_DEVICE);
        return 0;

      default:
        g_source = VG_SRC_NONE;
        return 0;
    }
}

int vg_capture_read(int16_t *buf, size_t nsamples)
{
  if (buf == NULL || nsamples == 0)
    {
      return -1;
    }

  if (g_hold_until_ms != 0)
    {
      if (vg_now_ms() < g_hold_until_ms)
        {
          return 0;
        }

      g_hold_until_ms = 0;
    }

  if (g_paused)
    {
      return 0;
    }

  switch (g_source)
    {
      case VG_SRC_WAV:
        return vg_wav_read(buf, nsamples);

      case VG_SRC_MIC:
        {
          ssize_t n = read(g_mic_fd, buf, nsamples * sizeof(int16_t));

          if (n <= 0)
            {
              return (int)n;
            }

          return (int)(n / (ssize_t)sizeof(int16_t));
        }

      default:
        return 0;
    }
}

void vg_capture_close(void)
{
  if (g_wav != NULL)
    {
      fclose(g_wav);
      g_wav = NULL;
    }

  if (g_mic_fd >= 0)
    {
      close(g_mic_fd);
      g_mic_fd = -1;
    }

  g_source = VG_SRC_NONE;
}

vg_source_t vg_capture_source(void)
{
  return g_source;
}

const char *vg_capture_source_name(void)
{
  switch (g_source)
    {
      case VG_SRC_MIC:
        return "板载麦克风";

      case VG_SRC_WAV:
        return "wav 回放";

      default:
        return "无音频源(模拟注入)";
    }
}

void vg_capture_pause(const char *reason)
{
  g_paused = true;
  vg_strlcpy(g_pause_reason, reason != NULL ? reason : "守护已暂停", sizeof(g_pause_reason));
}

void vg_capture_resume(void)
{
  g_paused = false;
  g_pause_reason[0] = '\0';
}

bool vg_capture_paused(void)
{
  return g_paused;
}

const char *vg_capture_pause_reason(void)
{
  return g_pause_reason[0] != '\0' ? g_pause_reason : "守护中";
}

void vg_capture_hold_for_playback(uint32_t max_ms)
{
  /* PRD-04：不支持录放并发时的时分方案，单次中断上限 2 秒 */

  if (max_ms > 2000)
    {
      max_ms = 2000;
    }

  g_hold_until_ms = vg_now_ms() + max_ms;
}

void vg_capture_release_playback(void)
{
  g_hold_until_ms = 0;
}
