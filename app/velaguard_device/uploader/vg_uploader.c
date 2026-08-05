/****************************************************************************
 * 安聆 VelaGuard - 有界持久化事件 outbox (PRD-05 / 系统设计阶段 1-2)
 *
 * 入队接口只复制结构化消息并唤醒后续 tick，不在状态机回调中联网或执行
 * 文件 I/O。tick 所属的上传任务负责快照提交、网络发送和 ACK 回收。
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "velaguard/vg_config.h"
#include "velaguard/vg_json.h"
#include "velaguard/vg_notifier.h"
#include "velaguard/vg_time.h"
#include "velaguard/vg_transport.h"
#include "velaguard/vg_uploader.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_UPLOAD_QUEUE_MAX 64
#define VG_PAYLOAD_MAX      1536
#define VG_UPLOAD_BYTES_MAX (VG_UPLOAD_QUEUE_MAX * VG_PAYLOAD_MAX)
#define VG_P0_RESERVE_BYTES (32 * VG_PAYLOAD_MAX)
#define VG_BACKOFF_MAX_SEC  900

#define VG_OUTBOX_MAGIC   0x56474f31u /* "VGO1" */
#define VG_OUTBOX_VERSION 1
#define VG_IDENTITY_MAGIC 0x56474931u /* "VGI1" */
#define VG_IDENTITY_VERSION 1

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef enum
{
  VG_UPLOAD_P2 = 0,
  VG_UPLOAD_P1,
  VG_UPLOAD_P0
} vg_upload_priority_t;

typedef struct
{
  bool     used;
  uint8_t  priority;
  uint16_t payload_len;
  char     event_id[VG_EVENT_ID_LEN];
  char     message_id[VG_MESSAGE_ID_LEN];
  uint32_t event_revision;
  uint64_t device_seq;
  uint64_t enqueued_ms;
  uint64_t next_try_ms;
  uint32_t attempts;
  int32_t  last_error;
  char     payload[VG_PAYLOAD_MAX];
} vg_upload_item_t;

typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t generation;
  uint32_t count;
  uint32_t payload_bytes;
  uint32_t crc32;
  char     device_epoch[VG_DEVICE_EPOCH_LEN];
} vg_outbox_header_t;

typedef struct
{
  uint8_t  used;
  uint8_t  priority;
  uint16_t payload_len;
  uint32_t attempts;
  int32_t  last_error;
  uint32_t event_revision;
  uint64_t device_seq;
  uint64_t enqueued_ms;
  uint64_t next_try_ms;
  char     event_id[VG_EVENT_ID_LEN];
  char     message_id[VG_MESSAGE_ID_LEN];
  char     payload[VG_PAYLOAD_MAX];
} vg_upload_disk_item_t;

typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t boot_counter;
  uint64_t next_seq;
  char     device_epoch[VG_DEVICE_EPOCH_LEN];
  uint32_t crc32;
} vg_identity_disk_t;

typedef enum
{
  VG_SEND_OK = 0,
  VG_SEND_RETRY,
  VG_SEND_PERMANENT
} vg_send_result_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_upload_item_t g_queue[VG_UPLOAD_QUEUE_MAX];
static vg_upload_item_t g_previous_queue[VG_UPLOAD_QUEUE_MAX];
static char             g_pending_path[VG_PATH_LEN + 32];
static char             g_pending_tmp_path[VG_PATH_LEN + 40];
static char             g_pending_previous_path[VG_PATH_LEN + 40];
static char             g_identity_path[VG_PATH_LEN + 32];
static char             g_dead_letter_path[VG_PATH_LEN + 40];
static char             g_device_epoch[VG_DEVICE_EPOCH_LEN];
static uint64_t         g_device_seq;
static uint32_t         g_generation;
static uint32_t         g_boot_counter;
static uint32_t         g_sent;
static uint32_t         g_failed;
static uint32_t         g_dropped;
static uint32_t         g_dead_letter;
static uint32_t         g_storage_errors;
static bool             g_storage_saturated;
static bool             g_online;
static bool             g_dirty;
static bool             g_inited;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t vg_crc32_update(uint32_t crc, const void *data, size_t len)
{
  const unsigned char *p = (const unsigned char *)data;
  size_t i;

  for (i = 0; i < len; i++)
    {
      unsigned int bit;

      crc ^= p[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int)(crc & 1));
        }
    }

  return crc;
}

static uint32_t vg_crc32(const void *data, size_t len)
{
  return ~vg_crc32_update(0xffffffffu, data, len);
}

static bool vg_id_valid(const char *s, size_t capacity)
{
  size_t i;

  if (s == NULL || capacity == 0 || memchr(s, '\0', capacity) == NULL ||
      s[0] == '\0')
    {
      return false;
    }

  for (i = 0; s[i] != '\0'; i++)
    {
      unsigned char c = (unsigned char)s[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
        {
          return false;
        }
    }

  return true;
}

static int vg_sync_file(FILE *fp)
{
  int fd;

  if (fp == NULL || fflush(fp) != 0)
    {
      return -1;
    }

  fd = fileno(fp);
  return fd >= 0 && fsync(fd) == 0 ? 0 : -1;
}

static void vg_mark_storage_error(void)
{
  g_storage_errors++;
  g_storage_saturated = true;
}

static int vg_effective_queue_max(void)
{
  uint32_t configured = vg_config()->upload_queue_max;

  if (configured == 0 || configured > VG_UPLOAD_QUEUE_MAX)
    {
      return VG_UPLOAD_QUEUE_MAX;
    }

  return (int)configured;
}

static uint32_t vg_queue_count(void)
{
  uint32_t count = 0;
  int i;

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (g_queue[i].used)
        {
          count++;
        }
    }

  return count;
}

static size_t vg_queue_bytes(void)
{
  size_t bytes = 0;
  int i;

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (g_queue[i].used)
        {
          bytes += g_queue[i].payload_len;
        }
    }

  return bytes;
}

static int vg_priority_for_event(const vg_safety_event_t *evt)
{
  if (evt == NULL)
    {
      return VG_UPLOAD_P2;
    }

  if (evt->level == VG_LEVEL_EMERGENCY ||
      evt->upload_reason == VG_UPLOAD_ESCALATED ||
      evt->local_status == VG_STATUS_HANDLED ||
      evt->local_status == VG_STATUS_FALSE_ALARM ||
      evt->local_status == VG_STATUS_NO_RESPONSE)
    {
      return VG_UPLOAD_P0;
    }

  if (evt->level == VG_LEVEL_WARNING)
    {
      return VG_UPLOAD_P1;
    }

  return VG_UPLOAD_P2;
}

static int vg_find_duplicate(const char *event_id, uint32_t revision)
{
  int i;

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (g_queue[i].used &&
          g_queue[i].event_revision == revision &&
          strcmp(g_queue[i].event_id, event_id) == 0)
        {
          return i;
        }
    }

  return -1;
}

static int vg_find_free(void)
{
  int i;

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (!g_queue[i].used)
        {
          return i;
        }
    }

  return -1;
}

static int vg_find_eviction(int priority, size_t payload_len)
{
  int candidate = -1;
  int i;
  bool needs_space;
  bool over_bytes = vg_queue_bytes() + payload_len > VG_UPLOAD_BYTES_MAX;
  bool over_normal_reserve = priority < VG_UPLOAD_P0 &&
                             vg_queue_bytes() + payload_len >
                             VG_UPLOAD_BYTES_MAX - VG_P0_RESERVE_BYTES;

  needs_space = vg_queue_count() >= (uint32_t)vg_effective_queue_max() ||
                over_bytes || over_normal_reserve;
  if (!needs_space)
    {
      return -1;
    }

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (!g_queue[i].used || g_queue[i].priority >= priority)
        {
          continue;
        }

      if (candidate < 0 ||
          g_queue[i].enqueued_ms < g_queue[candidate].enqueued_ms)
        {
          candidate = i;
        }
    }

  return candidate >= 0 ? candidate : -2;
}

static void vg_make_random_bytes(unsigned char *out, size_t len)
{
  int fd;
  ssize_t got;
  uint64_t fallback;

  memset(out, 0, len);
  fd = open("/dev/urandom", O_RDONLY);
  got = fd >= 0 ? read(fd, out, len) : -1;
  if (fd >= 0)
    {
      close(fd);
    }

  if (got == (ssize_t)len)
    {
      return;
    }

  fallback = vg_now_ms() ^ (uint64_t)vg_wall_sec() ^
             ((uint64_t)(uintptr_t)out << 7);
  memcpy(out, &fallback, len < sizeof(fallback) ? len : sizeof(fallback));
}

static int vg_identity_save(uint32_t boot_counter)
{
  vg_identity_disk_t disk;
  FILE *fp;

  memset(&disk, 0, sizeof(disk));
  disk.magic = VG_IDENTITY_MAGIC;
  disk.version = VG_IDENTITY_VERSION;
  disk.boot_counter = boot_counter;
  disk.next_seq = g_device_seq;
  vg_strlcpy(disk.device_epoch, g_device_epoch, sizeof(disk.device_epoch));
  disk.crc32 = 0;
  disk.crc32 = vg_crc32(&disk, sizeof(disk));

  fp = fopen(g_identity_path, "wb");
  if (fp == NULL || fwrite(&disk, 1, sizeof(disk), fp) != sizeof(disk) ||
      vg_sync_file(fp) < 0)
    {
      if (fp != NULL)
        {
          fclose(fp);
        }
      return -1;
    }

  fclose(fp);
  return 0;
}

static int vg_identity_init(void)
{
  vg_identity_disk_t disk;
  uint32_t old_crc;
  uint32_t boot_counter = 0;
  unsigned char random_bytes[8];
  FILE *fp;

  fp = fopen(g_identity_path, "rb");
  if (fp != NULL && fread(&disk, 1, sizeof(disk), fp) == sizeof(disk))
    {
      old_crc = disk.crc32;
      disk.crc32 = 0;
      if (old_crc == vg_crc32(&disk, sizeof(disk)) &&
          disk.magic == VG_IDENTITY_MAGIC &&
          disk.version == VG_IDENTITY_VERSION)
        {
          boot_counter = disk.boot_counter;
        }
    }
  if (fp != NULL)
    {
      fclose(fp);
    }

  boot_counter++;
  g_boot_counter = boot_counter;
  vg_make_random_bytes(random_bytes, sizeof(random_bytes));
  {
    uint32_t random_word;
    memcpy(&random_word, random_bytes, sizeof(random_word));
    snprintf(g_device_epoch, sizeof(g_device_epoch), "b%04" PRIx32 "%08"
             PRIx32, boot_counter & 0xffffu, random_word);
  }
  g_device_seq = 0;
  return vg_identity_save(boot_counter);
}

static void vg_disk_from_item(const vg_upload_item_t *item,
                              vg_upload_disk_item_t *disk)
{
  memset(disk, 0, sizeof(*disk));
  disk->used = item->used ? 1 : 0;
  disk->priority = item->priority;
  disk->payload_len = item->payload_len;
  disk->attempts = item->attempts;
  disk->last_error = item->last_error;
  disk->event_revision = item->event_revision;
  disk->device_seq = item->device_seq;
  disk->enqueued_ms = item->enqueued_ms;
  disk->next_try_ms = item->next_try_ms;
  vg_strlcpy(disk->event_id, item->event_id, sizeof(disk->event_id));
  vg_strlcpy(disk->message_id, item->message_id, sizeof(disk->message_id));
  if (item->payload_len > 0 && item->payload_len < sizeof(disk->payload))
    {
      memcpy(disk->payload, item->payload, item->payload_len);
    }
}

static int vg_item_from_disk(const vg_upload_disk_item_t *disk,
                             vg_upload_item_t *item)
{
  vg_event_envelope_t envelope;

  if (disk == NULL || item == NULL || disk->used == 0 ||
      disk->payload_len == 0 || disk->payload_len >= VG_PAYLOAD_MAX ||
      disk->priority > VG_UPLOAD_P0 ||
      disk->event_revision == 0 ||
      disk->payload[disk->payload_len] != '\0')
    {
      return -1;
    }

  if (vg_event_envelope_from_json(disk->payload, &envelope) < 0 ||
      strcmp(envelope.event_id, disk->event_id) != 0 ||
      strcmp(envelope.message_id, disk->message_id) != 0 ||
      envelope.event_revision != disk->event_revision ||
      envelope.device_seq != disk->device_seq)
    {
      return -1;
    }

  memset(item, 0, sizeof(*item));
  item->used = true;
  item->priority = disk->priority;
  item->payload_len = disk->payload_len;
  item->attempts = disk->attempts;
  item->last_error = disk->last_error;
  item->event_revision = disk->event_revision;
  item->device_seq = disk->device_seq;
  item->enqueued_ms = disk->enqueued_ms;
  item->next_try_ms = disk->next_try_ms;
  vg_strlcpy(item->event_id, disk->event_id, sizeof(item->event_id));
  vg_strlcpy(item->message_id, disk->message_id, sizeof(item->message_id));
  memcpy(item->payload, disk->payload, disk->payload_len + 1);
  return 0;
}

static int vg_outbox_load_file(const char *path, vg_upload_item_t *items,
                               uint32_t *generation)
{
  vg_outbox_header_t header;
  vg_upload_disk_item_t disk;
  uint32_t saved_crc;
  uint32_t crc;
  uint32_t count = 0;
  uint32_t payload_bytes = 0;
  uint32_t i;
  FILE *fp;

  fp = fopen(path, "rb");
  if (fp == NULL)
    {
      return -1;
    }

  if (fread(&header, 1, sizeof(header), fp) != sizeof(header) ||
      header.magic != VG_OUTBOX_MAGIC ||
      header.version != VG_OUTBOX_VERSION ||
      header.header_size != sizeof(header) ||
      header.count > VG_UPLOAD_QUEUE_MAX ||
      header.payload_bytes > VG_UPLOAD_BYTES_MAX ||
      !vg_id_valid(header.device_epoch, sizeof(header.device_epoch)))
    {
      fclose(fp);
      return -1;
    }

  saved_crc = header.crc32;
  header.crc32 = 0;
  crc = vg_crc32_update(0xffffffffu, &header, sizeof(header));
  memset(items, 0, sizeof(vg_upload_item_t) * VG_UPLOAD_QUEUE_MAX);

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (fread(&disk, 1, sizeof(disk), fp) != sizeof(disk))
        {
          fclose(fp);
          return -1;
        }

      crc = vg_crc32_update(crc, &disk, sizeof(disk));
      if (disk.used != 0)
        {
          if (count >= VG_UPLOAD_QUEUE_MAX ||
              vg_item_from_disk(&disk, &items[count]) < 0)
            {
              fclose(fp);
              return -1;
            }

          count++;
          payload_bytes += disk.payload_len;
        }
    }

  fclose(fp);
  if ((~crc) != saved_crc || count != header.count ||
      payload_bytes != header.payload_bytes)
    {
      return -1;
    }

  *generation = header.generation;
  return 0;
}

static int vg_pending_save(void)
{
  vg_outbox_header_t header;
  vg_upload_disk_item_t disk;
  uint32_t crc;
  uint32_t count = 0;
  uint32_t payload_bytes = 0;
  int i;
  FILE *fp;

  if (g_pending_path[0] == '\0')
    {
      return -1;
    }

  memset(&header, 0, sizeof(header));
  header.magic = VG_OUTBOX_MAGIC;
  header.version = VG_OUTBOX_VERSION;
  header.header_size = sizeof(header);
  header.generation = ++g_generation;
  vg_strlcpy(header.device_epoch, g_device_epoch, sizeof(header.device_epoch));

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (g_queue[i].used)
        {
          count++;
          payload_bytes += g_queue[i].payload_len;
        }
    }
  header.count = count;
  header.payload_bytes = payload_bytes;
  header.crc32 = 0;
  crc = vg_crc32_update(0xffffffffu, &header, sizeof(header));

  fp = fopen(g_pending_tmp_path, "wb");
  if (fp == NULL || fwrite(&header, 1, sizeof(header), fp) != sizeof(header))
    {
      if (fp != NULL)
        {
          fclose(fp);
        }
      vg_mark_storage_error();
      return -1;
    }

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      vg_disk_from_item(&g_queue[i], &disk);
      if (fwrite(&disk, 1, sizeof(disk), fp) != sizeof(disk))
        {
          fclose(fp);
          vg_mark_storage_error();
          return -1;
        }
      crc = vg_crc32_update(crc, &disk, sizeof(disk));
    }

  header.crc32 = ~crc;
  if (fseek(fp, 0, SEEK_SET) != 0 ||
      fwrite(&header, 1, sizeof(header), fp) != sizeof(header) ||
      vg_sync_file(fp) < 0)
    {
      fclose(fp);
      vg_mark_storage_error();
      return -1;
    }
  fclose(fp);

  if (access(g_pending_path, F_OK) == 0)
    {
      (void)rename(g_pending_path, g_pending_previous_path);
    }
  if (rename(g_pending_tmp_path, g_pending_path) < 0)
    {
      vg_mark_storage_error();
      return -1;
    }

  g_dirty = false;
  g_storage_saturated = false;
  return 0;
}

static void vg_pending_load(void)
{
  uint32_t active_generation = 0;
  uint32_t previous_generation = 0;
  int active_ok;
  int previous_ok;

  memset(g_queue, 0, sizeof(g_queue));
  active_ok = vg_outbox_load_file(g_pending_path, g_queue, &active_generation);
  previous_ok = vg_outbox_load_file(g_pending_previous_path, g_previous_queue,
                                    &previous_generation);

  if (active_ok == 0 && (previous_ok < 0 ||
                         active_generation >= previous_generation))
    {
      g_generation = active_generation;
    }
  else if (previous_ok == 0)
    {
      memcpy(g_queue, g_previous_queue, sizeof(g_queue));
      g_generation = previous_generation;
    }
  else if (active_ok < 0 && previous_ok < 0 &&
           (access(g_pending_path, F_OK) == 0 ||
            access(g_pending_previous_path, F_OK) == 0))
    {
      memset(g_queue, 0, sizeof(g_queue));
      fprintf(stderr, "[velaguard] outbox 快照损坏，进入存储降级\n");
      vg_mark_storage_error();
    }
  else if (active_ok < 0)
    {
      memset(g_queue, 0, sizeof(g_queue));
    }

  if (vg_queue_count() > 0)
    {
      printf("[velaguard] 从断点恢复 %" PRIu32 " 条待发送通知\n",
             vg_queue_count());
    }
}

static void vg_dead_letter_append(const vg_upload_item_t *item, int error)
{
  FILE *fp;

  fp = fopen(g_dead_letter_path, "a");
  if (fp == NULL)
    {
      vg_mark_storage_error();
      return;
    }

  if (fprintf(fp, "{\"eventId\":\"%s\",\"messageId\":\"%s\","
              "\"eventRevision\":%" PRIu32 ",\"attempts\":%" PRIu32
              ",\"error\":%d}\n",
              item->event_id, item->message_id, item->event_revision,
              item->attempts, error) < 0 ||
      vg_sync_file(fp) < 0)
    {
      vg_mark_storage_error();
    }
  fclose(fp);
}

static bool vg_ack_matches(const vg_upload_item_t *item,
                           const char *response)
{
  char schema[24];
  char message_id[VG_MESSAGE_ID_LEN];
  char event_id[VG_EVENT_ID_LEN];
  char device_id[VG_DEVICE_ID_LEN];
  bool accepted;
  int revision;

  return response != NULL &&
         vg_json_get_str(response, "schema", schema, sizeof(schema)) == 0 &&
         strcmp(schema, VG_ACK_SCHEMA) == 0 &&
         vg_json_get_str(response, "messageId", message_id,
                         sizeof(message_id)) == 0 &&
         strcmp(message_id, item->message_id) == 0 &&
         vg_json_get_str(response, "eventId", event_id,
                         sizeof(event_id)) == 0 &&
         strcmp(event_id, item->event_id) == 0 &&
         vg_json_get_str(response, "deviceId", device_id,
                         sizeof(device_id)) == 0 &&
         strcmp(device_id, vg_config()->device_id) == 0 &&
         vg_json_get_int(response, "eventRevision", &revision) == 0 &&
         revision == (int)item->event_revision &&
         vg_json_get_bool(response, "accepted", &accepted) == 0 &&
         accepted;
}

static vg_send_result_t vg_try_send(vg_upload_item_t *item, int *error)
{
  vg_config_t *cfg = vg_config();
  char response[2048];
  int status = 0;
  int ret;

  if (cfg->console_tls)
    {
      if (error != NULL)
        {
          *error = VG_TRANSPORT_ERR_TLS_UNAVAILABLE;
        }
      return VG_SEND_PERMANENT;
    }

  if (!cfg->demo_mode)
    {
      if (error != NULL)
        {
          *error = -100;
        }
      return VG_SEND_PERMANENT;
    }

  /* https 地址在当前演示实现中明确拒绝，避免把明文 socket 冒充 TLS。 */
  ret = vg_transport_post_json(cfg->console_host, cfg->console_port,
                               cfg->console_path, false,
                               cfg->console_server_name,
                               cfg->console_ca_path, item->payload,
                               item->message_id,
                               cfg->device_token[0] != '\0' ?
                               cfg->device_token : NULL,
                               VG_EVENT_SCHEMA, response, sizeof(response),
                               &status, 3000);
  if (ret < 0)
    {
      if (error != NULL)
        {
          *error = ret;
        }
      return VG_SEND_RETRY;
    }

  if (status >= 200 && status < 300 && vg_ack_matches(item, response))
    {
      return VG_SEND_OK;
    }

  if (error != NULL)
    {
      *error = status != 0 ? status : -101;
    }

  if (status == 408 || status == 429 || status >= 500)
    {
      return VG_SEND_RETRY;
    }

  return VG_SEND_PERMANENT;
}

static uint64_t vg_backoff_ms(uint32_t attempts)
{
  uint64_t seconds = vg_config()->upload_retry_sec;
  uint32_t shift = attempts > 1 ? attempts - 1 : 0;

  if (shift > 10)
    {
      shift = 10;
    }

  seconds <<= shift;
  if (seconds > VG_BACKOFF_MAX_SEC)
    {
      seconds = VG_BACKOFF_MAX_SEC;
    }

  /* 确定性抖动足够避免多个设备同时重试，且不需要额外随机状态。 */
  seconds += attempts % 3;
  return seconds * 1000;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_uploader_init(void)
{
  vg_config_t *cfg = vg_config();
  char reason[96];

  if (vg_config_validate(cfg, reason, sizeof(reason)) < 0)
    {
      fprintf(stderr, "[velaguard] 上传配置拒绝: %s\n", reason);
      return -1;
    }

  memset(g_queue, 0, sizeof(g_queue));
  snprintf(g_pending_path, sizeof(g_pending_path),
           "%s/pending.outbox", cfg->data_dir);
  snprintf(g_pending_tmp_path, sizeof(g_pending_tmp_path),
           "%s/pending.outbox.tmp", cfg->data_dir);
  snprintf(g_pending_previous_path, sizeof(g_pending_previous_path),
           "%s/pending.outbox.previous", cfg->data_dir);
  snprintf(g_identity_path, sizeof(g_identity_path),
           "%s/device.identity", cfg->data_dir);
  snprintf(g_dead_letter_path, sizeof(g_dead_letter_path),
           "%s/dead-letter.jsonl", cfg->data_dir);

  (void)mkdir(cfg->data_dir, 0755);
  g_online = false;
  g_sent = 0;
  g_failed = 0;
  g_dropped = 0;
  g_dead_letter = 0;
  g_storage_errors = 0;
  g_storage_saturated = false;
  g_dirty = false;
  g_inited = true;

  if (vg_identity_init() < 0)
    {
      fprintf(stderr, "[velaguard] 设备纪元无法持久化，进入降级模式\n");
      vg_mark_storage_error();
    }
  vg_pending_load();
  return 0;
}

void vg_uploader_deinit(void)
{
  if (g_inited && g_dirty)
    {
      (void)vg_pending_save();
    }
  g_inited = false;
}

int vg_uploader_enqueue(const vg_safety_event_t *evt,
                        vg_upload_reason_t reason)
{
  return vg_uploader_enqueue_advice(evt, reason, NULL);
}

int vg_uploader_enqueue_advice(const vg_safety_event_t *evt,
                               vg_upload_reason_t reason,
                               const char *advice_in)
{
  vg_safety_event_t local;
  char advice[VG_ADVICE_LEN];
  vg_upload_item_t candidate;
  int duplicate;
  int slot;
  int evict;
  size_t payload_len;

  if (!g_inited || evt == NULL)
    {
      return -1;
    }

  local = *evt;
  local.upload_reason = reason;
  if (local.event_revision == 0)
    {
      local.event_revision = 1;
    }
  if (vg_event_validate(&local) < 0)
    {
      return -2;
    }

  if (advice_in != NULL && advice_in[0] != '\0')
    {
      vg_strlcpy(advice, advice_in, sizeof(advice));
      vg_utf8_trim(advice);
    }
  else
    {
      vg_notifier_build_advice(&local, advice, sizeof(advice));
    }

  duplicate = vg_find_duplicate(local.event_id, local.event_revision);
  memset(&candidate, 0, sizeof(candidate));
  candidate.used = true;
  candidate.priority = (uint8_t)vg_priority_for_event(&local);
  candidate.event_revision = local.event_revision;
  candidate.enqueued_ms = vg_now_ms();

  if (duplicate >= 0)
    {
      vg_upload_item_t *old = &g_queue[duplicate];

      vg_strlcpy(candidate.message_id, old->message_id,
                 sizeof(candidate.message_id));
      candidate.device_seq = old->device_seq;
      candidate.enqueued_ms = old->enqueued_ms;
      vg_strlcpy(candidate.event_id, old->event_id, sizeof(candidate.event_id));
    }
  else
    {
      unsigned long long seq;

      g_device_seq++;
      if (g_device_seq == 0)
        {
          return -3;
        }
      (void)vg_identity_save(g_boot_counter);
      seq = (unsigned long long)g_device_seq;
      snprintf(candidate.message_id, sizeof(candidate.message_id),
               "msg_%s_%" PRIu64, g_device_epoch, (uint64_t)seq);
      candidate.device_seq = g_device_seq;
      vg_strlcpy(candidate.event_id, local.event_id,
                 sizeof(candidate.event_id));
    }

  {
    char trace_id[VG_TRACE_ID_LEN];
    int n = snprintf(trace_id, sizeof(trace_id), "tr_%s_%" PRIu64,
                     g_device_epoch, candidate.device_seq);
    if (n < 0 || (size_t)n >= sizeof(trace_id) ||
        vg_event_envelope_to_json(&local, advice, candidate.message_id,
                                  g_device_epoch, candidate.device_seq,
                                  local.event_revision, trace_id,
                                  vg_now_ms(), candidate.payload,
                                  sizeof(candidate.payload)) < 0)
      {
        return -4;
      }
  }

  payload_len = strlen(candidate.payload);
  if (payload_len == 0 || payload_len >= sizeof(candidate.payload))
    {
      return -5;
    }
  candidate.payload_len = (uint16_t)payload_len;

  if (duplicate >= 0)
    {
      g_queue[duplicate] = candidate;
      g_dirty = true;
      return 0;
    }

  slot = vg_find_free();
  evict = vg_find_eviction(candidate.priority, payload_len);
  if (slot < 0 || evict >= 0)
    {
      if (evict >= 0)
        {
          slot = evict;
          g_dropped++;
        }
      else
        {
          if (candidate.priority == VG_UPLOAD_P0)
            {
              g_storage_saturated = true;
              g_storage_errors++;
              return -6;
            }

          g_dropped++;
          return 1;
        }
    }

  g_queue[slot] = candidate;
  g_dirty = true;
  return 0;
}

void vg_uploader_tick(void)
{
  uint64_t now = vg_now_ms();
  int selected = -1;
  int i;
  int error = 0;
  vg_send_result_t result;

  if (!g_inited)
    {
      return;
    }

  /* 只有快照提交成功后才允许网络任务发送，避免“已发送但未持久化”。 */
  if (g_dirty && vg_pending_save() < 0)
    {
      g_online = false;
      g_failed++;
      return;
    }

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (!g_queue[i].used || now < g_queue[i].next_try_ms)
        {
          continue;
        }

      if (selected < 0 ||
          g_queue[i].priority > g_queue[selected].priority ||
          (g_queue[i].priority == g_queue[selected].priority &&
           g_queue[i].enqueued_ms < g_queue[selected].enqueued_ms))
        {
          selected = i;
        }
    }

  if (selected < 0)
    {
      return;
    }

  result = vg_try_send(&g_queue[selected], &error);
  if (result == VG_SEND_OK)
    {
      g_queue[selected].used = false;
      g_sent++;
      g_online = true;
      g_dirty = true;
      (void)vg_pending_save();
      return;
    }

  g_queue[selected].attempts++;
  g_queue[selected].last_error = error;
  g_failed++;
  g_online = false;

  if (result == VG_SEND_PERMANENT)
    {
      vg_dead_letter_append(&g_queue[selected], error);
      g_queue[selected].used = false;
      g_dead_letter++;
      g_dirty = true;
      (void)vg_pending_save();
      return;
    }

  g_queue[selected].next_try_ms = now +
                                  vg_backoff_ms(g_queue[selected].attempts);
  g_dirty = true;
  (void)vg_pending_save();
}

int vg_uploader_pending(void)
{
  return (int)vg_queue_count();
}

bool vg_uploader_online(void)
{
  return g_online;
}

void vg_uploader_stats(uint32_t *sent, uint32_t *failed)
{
  if (sent != NULL)
    {
      *sent = g_sent;
    }
  if (failed != NULL)
    {
      *failed = g_failed;
    }
}

uint32_t vg_uploader_dropped(void)
{
  return g_dropped;
}

uint32_t vg_uploader_dead_letter(void)
{
  return g_dead_letter;
}

uint32_t vg_uploader_storage_errors(void)
{
  return g_storage_errors;
}

bool vg_uploader_storage_saturated(void)
{
  return g_storage_saturated;
}

void vg_uploader_flush(void)
{
  int i;

  if (!g_inited)
    {
      return;
    }

  for (i = 0; i < VG_UPLOAD_QUEUE_MAX; i++)
    {
      if (g_queue[i].used)
        {
          g_queue[i].next_try_ms = 0;
        }
    }

  vg_uploader_tick();
}
