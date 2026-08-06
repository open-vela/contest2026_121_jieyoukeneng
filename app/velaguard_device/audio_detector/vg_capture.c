/****************************************************************************
 * 安聆 VelaGuard - 音频采集抽象实现 (PRD-01)
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mqueue.h>
#include <sys/ioctl.h>

#include <nuttx/audio/audio.h>

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
static mqd_t       g_mic_mq = (mqd_t)-1;
static struct ap_buffer_s **g_mic_buffers;
static unsigned int g_mic_nbuffers;
static pthread_t    g_mic_thread;
static volatile bool g_mic_running;
static pthread_mutex_t g_mic_lock = PTHREAD_MUTEX_INITIALIZER;
#define VG_MIC_RING_SAMPLES 32768
static int16_t      g_mic_ring[VG_MIC_RING_SAMPLES];
static size_t       g_mic_ring_read;
static size_t       g_mic_ring_write;
static size_t       g_mic_ring_count;
static bool        g_paused;
static char        g_pause_reason[48];
static uint64_t    g_hold_until_ms;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void vg_mic_ring_reset(void)
{
  pthread_mutex_lock(&g_mic_lock);
  g_mic_ring_read = 0;
  g_mic_ring_write = 0;
  g_mic_ring_count = 0;
  pthread_mutex_unlock(&g_mic_lock);
}

static void vg_mic_ring_write(const int16_t *samples, size_t count)
{
  size_t i;

  pthread_mutex_lock(&g_mic_lock);
  for (i = 0; i < count; i++)
    {
      if (g_mic_ring_count == VG_MIC_RING_SAMPLES)
        {
          g_mic_ring_read = (g_mic_ring_read + 1) % VG_MIC_RING_SAMPLES;
          g_mic_ring_count--;
        }

      g_mic_ring[g_mic_ring_write] = samples[i];
      g_mic_ring_write = (g_mic_ring_write + 1) % VG_MIC_RING_SAMPLES;
      g_mic_ring_count++;
    }
  pthread_mutex_unlock(&g_mic_lock);
}

static int vg_mic_enqueue(struct ap_buffer_s *buffer)
{
  struct audio_buf_desc_s desc;

  buffer->nbytes = buffer->nmaxbytes;
  buffer->curbyte = 0;
  memset(&desc, 0, sizeof(desc));
  desc.numbytes = buffer->nbytes;
  desc.u.buffer = buffer;
  return ioctl(g_mic_fd, AUDIOIOC_ENQUEUEBUFFER,
               (unsigned long)&desc);
}

static void *vg_mic_worker(void *arg)
{
  struct audio_msg_s msg;
  unsigned int prio;

  (void)arg;
  while (g_mic_running)
    {
      struct timespec deadline;

      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += 100 * 1000 * 1000;
      if (deadline.tv_nsec >= 1000000000L)
        {
          deadline.tv_sec++;
          deadline.tv_nsec -= 1000000000L;
        }

      if (mq_timedreceive(g_mic_mq, (char *)&msg, sizeof(msg), &prio,
                          &deadline) != sizeof(msg))
        {
          continue;
        }

      if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr != NULL)
        {
          struct ap_buffer_s *buffer = msg.u.ptr;
          size_t samples = buffer->nbytes / sizeof(int16_t);
          size_t max_samples = buffer->nmaxbytes / sizeof(int16_t);

          if (samples > max_samples) samples = max_samples;

          if (samples > 0 && buffer->samp != NULL)
            {
              vg_mic_ring_write((const int16_t *)buffer->samp, samples);
            }

          if (g_mic_running)
            {
              (void)vg_mic_enqueue(buffer);
            }
        }
      else if (msg.msg_id == AUDIO_MSG_COMPLETE ||
               msg.msg_id == AUDIO_MSG_STOP)
        {
          break;
        }
    }

  return NULL;
}

static void vg_mic_release(void)
{
  struct audio_buf_desc_s desc;
  unsigned int i;

  if (g_mic_fd < 0)
    {
      return;
    }

  (void)ioctl(g_mic_fd, AUDIOIOC_STOP, 0);
  if (g_mic_mq != (mqd_t)-1)
    {
      (void)ioctl(g_mic_fd, AUDIOIOC_UNREGISTERMQ,
                  (unsigned long)g_mic_mq);
    }
  memset(&desc, 0, sizeof(desc));
  for (i = 0; i < g_mic_nbuffers; i++)
    {
      if (g_mic_buffers != NULL && g_mic_buffers[i] != NULL)
        {
          desc.u.buffer = g_mic_buffers[i];
          (void)ioctl(g_mic_fd, AUDIOIOC_FREEBUFFER,
                      (unsigned long)&desc);
        }
    }

  free(g_mic_buffers);
  g_mic_buffers = NULL;
  g_mic_nbuffers = 0;
  if (g_mic_mq != (mqd_t)-1)
    {
      (void)mq_close(g_mic_mq);
      (void)mq_unlink("/tmp/velaguard_mic");
      g_mic_mq = (mqd_t)-1;
    }
  (void)ioctl(g_mic_fd, AUDIOIOC_RELEASE, 0);
  close(g_mic_fd);
  g_mic_fd = -1;
}

static int vg_mic_open(const char *device)
{
  struct audio_caps_desc_s cap_desc;
  struct ap_buffer_info_s buffer_info;
  struct audio_buf_desc_s buffer_desc;
  struct mq_attr attr;
  unsigned int i;

  g_mic_fd = open(device, O_RDWR | O_CLOEXEC);
  if (g_mic_fd < 0 || ioctl(g_mic_fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      vg_mic_release();
      return -1;
    }

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type = AUDIO_TYPE_INPUT;
  cap_desc.caps.ac_channels = 1;
  cap_desc.caps.ac_controls.hw[0] = VG_SAMPLE_RATE;
  cap_desc.caps.ac_controls.b[2] = 16;
  cap_desc.caps.ac_subtype = AUDIO_FMT_PCM;
  if (ioctl(g_mic_fd, AUDIOIOC_CONFIGURE,
            (unsigned long)&cap_desc) < 0)
    {
      vg_mic_release();
      return -1;
    }

  memset(&buffer_info, 0, sizeof(buffer_info));
  if (ioctl(g_mic_fd, AUDIOIOC_GETBUFFERINFO,
            (unsigned long)&buffer_info) < 0 ||
      buffer_info.nbuffers == 0 || buffer_info.buffer_size == 0)
    {
      buffer_info.nbuffers = 4;
      buffer_info.buffer_size = 2048;
    }

  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg = buffer_info.nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  (void)mq_unlink("/tmp/velaguard_mic");
  g_mic_mq = mq_open("/tmp/velaguard_mic", O_RDWR | O_CREAT, 0644, &attr);
  if (g_mic_mq == (mqd_t)-1 ||
      ioctl(g_mic_fd, AUDIOIOC_REGISTERMQ, (unsigned long)g_mic_mq) < 0)
    {
      vg_mic_release();
      return -1;
    }

  g_mic_nbuffers = buffer_info.nbuffers;
  g_mic_buffers = calloc(g_mic_nbuffers, sizeof(*g_mic_buffers));
  if (g_mic_buffers == NULL)
    {
      vg_mic_release();
      return -1;
    }

  for (i = 0; i < g_mic_nbuffers; i++)
    {
      memset(&buffer_desc, 0, sizeof(buffer_desc));
      buffer_desc.numbytes = buffer_info.buffer_size;
      buffer_desc.u.pbuffer = &g_mic_buffers[i];
      if (ioctl(g_mic_fd, AUDIOIOC_ALLOCBUFFER,
                (unsigned long)&buffer_desc) < 0 ||
          vg_mic_enqueue(g_mic_buffers[i]) < 0)
        {
          vg_mic_release();
          return -1;
        }
    }

  vg_mic_ring_reset();
  g_mic_running = true;
  if (pthread_create(&g_mic_thread, NULL, vg_mic_worker, NULL) != 0)
    {
      g_mic_running = false;
      vg_mic_release();
      return -1;
    }

  if (ioctl(g_mic_fd, AUDIOIOC_START, 0) < 0)
    {
      struct audio_msg_s stop_msg;

      g_mic_running = false;
      memset(&stop_msg, 0, sizeof(stop_msg));
      stop_msg.msg_id = AUDIO_MSG_STOP;
      (void)mq_send(g_mic_mq, (const char *)&stop_msg,
                    sizeof(stop_msg), 0);
      pthread_join(g_mic_thread, NULL);
      vg_mic_release();
      return -1;
    }

  return 0;
}

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

  if (info->data_off == 0 || info->channels == 0 || info->rate == 0 ||
      info->channels > 8)
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
      int64_t byte_off = (int64_t)g_wav_info.data_off +
                         (int64_t)src_index * 2 * (int64_t)g_wav_info.channels;
      size_t framebytes = 2 * (size_t)g_wav_info.channels;
      int32_t acc = 0;
      int c;

      if (byte_off + (int64_t)framebytes >
          (int64_t)g_wav_info.data_off + g_wav_info.data_len)
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
        if (vg_mic_open(path != NULL ? path : CONFIG_VELAGUARD_PCM_DEVICE) < 0)
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
          size_t count = 0;

          pthread_mutex_lock(&g_mic_lock);
          while (count < nsamples && g_mic_ring_count > 0)
            {
              buf[count++] = g_mic_ring[g_mic_ring_read];
              g_mic_ring_read = (g_mic_ring_read + 1) % VG_MIC_RING_SAMPLES;
              g_mic_ring_count--;
            }
          pthread_mutex_unlock(&g_mic_lock);
          return (int)count;
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
      if (g_mic_running)
        {
          struct audio_msg_s stop_msg;

          g_mic_running = false;
          (void)ioctl(g_mic_fd, AUDIOIOC_STOP, 0);
          memset(&stop_msg, 0, sizeof(stop_msg));
          stop_msg.msg_id = AUDIO_MSG_STOP;
          (void)mq_send(g_mic_mq, (const char *)&stop_msg,
                        sizeof(stop_msg), 0);
          pthread_join(g_mic_thread, NULL);
        }
      vg_mic_release();
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
