/****************************************************************************
 * 安聆 VelaGuard - 本地事件日志实现 (PRD-05)
 *
 * 采用定长二进制环形文件：头部记录写指针与条数，其后是 100 条定长记录。
 * 每次写入只更新对应槽位与头部并 fsync，避免整文件重写，兼顾 SPI NOR 寿命
 * 与"写入即落盘"要求。
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "velaguard/vg_config.h"
#include "velaguard/vg_event_log.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_LOG_MAGIC       0x56474c32u  /* "VGL2" */
#define VG_LOG_SLOT_MAGIC  0x56534c31u  /* "VSL1" */
#define VG_LOG_VERSION     2

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct
{
  uint32_t magic;
  uint32_t version;
  uint32_t capacity;
  uint32_t head;      /* 下一个写入槽位 */
  uint32_t count;     /* 当前有效条数 */
  uint32_t total;     /* 累计写入条数 */
  uint32_t generation; /* 每次头部提交递增 */
  uint32_t crc32;
} vg_log_header_t;

typedef struct
{
  uint32_t magic;
  uint32_t sequence;
  vg_safety_event_t event;
  uint32_t crc32;
} vg_log_slot_disk_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static vg_log_header_t   g_hdr;
static vg_safety_event_t g_slots[VG_EVENT_LOG_CAPACITY];
static char              g_log_path[VG_PATH_LEN + 32];
static bool              g_ready = false;

/* 不持有长期打开的 fd：NuttX 的文件描述符属于任务，而 NSH 内建命令每次
 * 都是新任务，FLAT 构建下静态变量却跨任务保留——持久 fd 会在下一条命令里
 * 变成非法句柄（fdcheck 断言）。因此所有落盘操作都按需 open/close。
 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static off_t vg_slot_offset(uint32_t slot)
{
  return (off_t)sizeof(vg_log_header_t) +
         (off_t)slot * (off_t)sizeof(vg_log_slot_disk_t);
}

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
          crc = (crc >> 1) ^
                (0xedb88320u & (uint32_t)-(int)(crc & 1));
        }
    }

  return crc;
}

static uint32_t vg_crc32(const void *data, size_t len)
{
  return ~vg_crc32_update(0xffffffffu, data, len);
}

static bool vg_header_valid(const vg_log_header_t *header)
{
  vg_log_header_t copy;
  uint32_t saved;

  if (header == NULL || header->magic != VG_LOG_MAGIC ||
      header->version != VG_LOG_VERSION ||
      header->capacity != VG_EVENT_LOG_CAPACITY ||
      header->head >= VG_EVENT_LOG_CAPACITY ||
      header->count > VG_EVENT_LOG_CAPACITY)
    {
      return false;
    }

  copy = *header;
  saved = copy.crc32;
  copy.crc32 = 0;
  return saved == vg_crc32(&copy, sizeof(copy));
}

static bool vg_slot_valid(vg_log_slot_disk_t *slot)
{
  uint32_t saved;
  bool valid;

  if (slot == NULL || slot->magic != VG_LOG_SLOT_MAGIC ||
      vg_event_validate(&slot->event) != 0)
    {
      return false;
    }

  saved = slot->crc32;
  slot->crc32 = 0;
  valid = saved == vg_crc32(slot, sizeof(*slot));
  slot->crc32 = saved;
  return valid;
}

static bool vg_slot_is_active(uint32_t head, uint32_t count, uint32_t slot)
{
  uint32_t first;

  if (count >= VG_EVENT_LOG_CAPACITY)
    {
      return true;
    }

  first = (head + VG_EVENT_LOG_CAPACITY - count) % VG_EVENT_LOG_CAPACITY;
  if (first + count <= VG_EVENT_LOG_CAPACITY)
    {
      return slot >= first && slot < first + count;
    }

  return slot >= first || slot < (first + count) % VG_EVENT_LOG_CAPACITY;
}

/* 打开日志文件，失败返回 -1（此时降级为纯内存日志，本地告警不受影响） */

static int vg_log_open(int flags)
{
  if (g_log_path[0] == '\0')
    {
      return -1;
    }

  return open(g_log_path, flags, 0644);
}

static int vg_write_all(int fd, const void *buf, size_t len)
{
  const unsigned char *p = (const unsigned char *)buf;
  size_t written = 0;

  while (written < len)
    {
      ssize_t n = write(fd, p + written, len - written);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          return -1;
        }

      written += (size_t)n;
    }

  return 0;
}

static int vg_read_all(int fd, void *buf, size_t len)
{
  unsigned char *p = (unsigned char *)buf;
  size_t read_len = 0;

  while (read_len < len)
    {
      ssize_t n = read(fd, p + read_len, len - read_len);
      if (n < 0 && errno == EINTR)
        {
          continue;
        }

      if (n <= 0)
        {
          return -1;
        }

      read_len += (size_t)n;
    }

  return 0;
}

static int vg_write_at(off_t off, const void *buf, size_t len)
{
  int fd = vg_log_open(O_WRONLY | O_CREAT);
  int ret = -1;

  if (fd < 0)
    {
      return -1;
    }

  if (lseek(fd, off, SEEK_SET) >= 0 && vg_write_all(fd, buf, len) == 0)
    {
      fsync(fd);
      ret = 0;
    }

  close(fd);
  return ret;
}

static int vg_write_header(const vg_log_header_t *header)
{
  vg_log_header_t copy;

  if (header == NULL)
    {
      return -1;
    }

  copy = *header;
  copy.crc32 = 0;
  copy.crc32 = vg_crc32(&copy, sizeof(copy));
  return vg_write_at(0, &copy, sizeof(copy));
}

static int vg_write_slot(uint32_t slot)
{
  vg_log_slot_disk_t disk;

  if (slot >= VG_EVENT_LOG_CAPACITY)
    {
      return -1;
    }

  memset(&disk, 0, sizeof(disk));
  disk.magic = VG_LOG_SLOT_MAGIC;
  disk.sequence = g_hdr.total;
  disk.event = g_slots[slot];
  disk.crc32 = vg_crc32(&disk, sizeof(disk));
  return vg_write_at(vg_slot_offset(slot), &disk, sizeof(disk));
}

static void vg_reset_state(void)
{
  memset(&g_hdr, 0, sizeof(g_hdr));
  memset(g_slots, 0, sizeof(g_slots));
  g_hdr.magic = VG_LOG_MAGIC;
  g_hdr.version = VG_LOG_VERSION;
  g_hdr.capacity = VG_EVENT_LOG_CAPACITY;
}

/* 从"最新"索引换算到物理槽位 */

static int vg_slot_of(int idx_from_newest, uint32_t *slot)
{
  int32_t s;

  if (idx_from_newest < 0 || (uint32_t)idx_from_newest >= g_hdr.count)
    {
      return -1;
    }

  s = (int32_t)g_hdr.head - 1 - idx_from_newest;
  while (s < 0)
    {
      s += VG_EVENT_LOG_CAPACITY;
    }

  *slot = (uint32_t)s;
  return 0;
}

static int vg_find_slot(const char *event_id, uint32_t *slot)
{
  uint32_t i;

  if (event_id == NULL || event_id[0] == '\0')
    {
      return -1;
    }

  for (i = 0; i < g_hdr.count; i++)
    {
      uint32_t s;

      if (vg_slot_of((int)i, &s) == 0 &&
          strcmp(g_slots[s].event_id, event_id) == 0)
        {
          *slot = s;
          return 0;
        }
    }

  return -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_event_log_init(const char *dir)
{
  const char *base = (dir != NULL) ? dir : vg_config()->data_dir;
  vg_log_header_t disk;
  vg_log_slot_disk_t slot_disk;
  bool valid = true;
  uint32_t slot;
  int fd;

  vg_reset_state();

  mkdir(base, 0755);
  snprintf(g_log_path, sizeof(g_log_path), "%s/events.bin", base);

  fd = vg_log_open(O_RDONLY);
  if (fd < 0)
    {
      /* 首次运行：创建文件并写入空头部 */

      vg_reset_state();
      if (vg_write_header(&g_hdr) < 0)
        {
          /* 存储不可用时降级为纯内存日志，本地提醒能力不受影响 */

          fprintf(stderr,
                  "[velaguard] 事件日志文件不可用(%s)，降级为内存日志\n",
                  strerror(errno));
        }

      g_ready = true;
      return 0;
    }

  if (vg_read_all(fd, &disk, sizeof(disk)) == 0 &&
      vg_header_valid(&disk) &&
      lseek(fd, vg_slot_offset(0), SEEK_SET) >= 0)
    {
      g_hdr = disk;
      memset(g_slots, 0, sizeof(g_slots));
      for (slot = 0; slot < VG_EVENT_LOG_CAPACITY; slot++)
        {
          if (!vg_slot_is_active(g_hdr.head, g_hdr.count, slot))
            {
              continue;
            }

          if (lseek(fd, vg_slot_offset(slot), SEEK_SET) < 0 ||
              vg_read_all(fd, &slot_disk, sizeof(slot_disk)) < 0)
            {
              valid = false;
              break;
            }

          if (!vg_slot_valid(&slot_disk))
            {
              valid = false;
              break;
            }

          g_slots[slot] = slot_disk.event;
        }

      close(fd);
    }
  else
    {
      valid = false;
      close(fd);
    }

  if (!valid)
    {
      /* 新建或损坏：重建文件 */

      if (truncate(g_log_path, 0) < 0)
        {
          fprintf(stderr, "[velaguard] 事件日志重建失败\n");
        }

      vg_reset_state();
      vg_write_header(&g_hdr);
    }

  g_ready = true;
  return 0;
}

void vg_event_log_deinit(void)
{
  g_ready = false;
}

int vg_event_log_append(const vg_safety_event_t *evt)
{
  vg_log_header_t next;
  vg_safety_event_t old_event;
  uint32_t slot;

  if (!g_ready || evt == NULL || vg_event_validate(evt) != 0)
    {
      return -1;
    }

  slot = g_hdr.head;
  old_event = g_slots[slot];
  g_slots[slot] = *evt;

  next = g_hdr;
  next.head = (next.head + 1) % VG_EVENT_LOG_CAPACITY;
  if (next.count < VG_EVENT_LOG_CAPACITY)
    {
      next.count++;
    }

  next.total++;
  next.generation++;

  if (vg_write_slot(slot) < 0 || vg_write_header(&next) < 0)
    {
      g_slots[slot] = old_event;
      return -1;
    }

  g_hdr = next;
  return 0;
}

int vg_event_log_update(const vg_safety_event_t *evt)
{
  vg_safety_event_t old_event;
  uint32_t slot;

  if (!g_ready || evt == NULL)
    {
      return -1;
    }

  if (vg_find_slot(evt->event_id, &slot) < 0)
    {
      return -1;
    }

  old_event = g_slots[slot];
  g_slots[slot] = *evt;
  if (vg_write_slot(slot) < 0)
    {
      g_slots[slot] = old_event;
      return -1;
    }

  return 0;
}

int vg_event_log_put(const vg_safety_event_t *evt)
{
  if (vg_event_log_update(evt) == 0)
    {
      return 0;
    }

  return vg_event_log_append(evt);
}

int vg_event_log_count(void)
{
  return (int)g_hdr.count;
}

uint32_t vg_event_log_total(void)
{
  return g_hdr.total;
}

int vg_event_log_get(int idx, vg_safety_event_t *out)
{
  uint32_t slot;

  if (out == NULL || vg_slot_of(idx, &slot) < 0)
    {
      return -1;
    }

  *out = g_slots[slot];
  return 0;
}

int vg_event_log_find(const char *event_id, vg_safety_event_t *out)
{
  uint32_t slot;

  if (out == NULL || vg_find_slot(event_id, &slot) < 0)
    {
      return -1;
    }

  *out = g_slots[slot];
  return 0;
}

int vg_event_log_clear(void)
{
  vg_log_header_t old_header = g_hdr;
  vg_safety_event_t old_slots[VG_EVENT_LOG_CAPACITY];

  memcpy(old_slots, g_slots, sizeof(old_slots));
  vg_reset_state();

  if (g_log_path[0] != '\0' && truncate(g_log_path, 0) < 0)
    {
      fprintf(stderr, "[velaguard] 事件日志清空失败\n");
    }

  if (vg_write_header(&g_hdr) < 0)
    {
      g_hdr = old_header;
      memcpy(g_slots, old_slots, sizeof(g_slots));
      return -1;
    }

  return 0;
}

int vg_event_log_to_json(int n, char *buf, size_t len)
{
  size_t pos = 0;
  int i;

  if (buf == NULL || len < 8)
    {
      return -1;
    }

  if (n <= 0 || n > (int)g_hdr.count)
    {
      n = (int)g_hdr.count;
    }

  buf[pos++] = '[';

  for (i = 0; i < n; i++)
    {
      vg_safety_event_t evt;
      int written;

      if (vg_event_log_get(i, &evt) < 0)
        {
          break;
        }

      if (i > 0)
        {
          if (pos + 1 >= len)
            {
              break;
            }

          buf[pos++] = ',';
        }

      written = vg_event_to_json(&evt, buf + pos, len - pos - 2);
      if (written < 0)
        {
          /* 缓冲区不足：回退逗号并结束 */

          if (i > 0)
            {
              pos--;
            }

          break;
        }

      pos += (size_t)written;
    }

  buf[pos++] = ']';
  buf[pos] = '\0';
  return (int)pos;
}
