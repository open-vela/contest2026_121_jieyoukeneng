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

#define VG_LOG_MAGIC    0x56474c31u  /* "VGL1" */
#define VG_LOG_VERSION  1

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
} vg_log_header_t;

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
         (off_t)slot * (off_t)sizeof(vg_safety_event_t);
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

static int vg_write_at(off_t off, const void *buf, size_t len)
{
  int fd = vg_log_open(O_WRONLY | O_CREAT);
  int ret = -1;

  if (fd < 0)
    {
      return -1;
    }

  if (lseek(fd, off, SEEK_SET) >= 0 &&
      write(fd, buf, len) == (ssize_t)len)
    {
      fsync(fd);
      ret = 0;
    }

  close(fd);
  return ret;
}

static int vg_write_header(void)
{
  return vg_write_at(0, &g_hdr, sizeof(g_hdr));
}

static int vg_write_slot(uint32_t slot)
{
  if (slot >= VG_EVENT_LOG_CAPACITY)
    {
      return -1;
    }

  return vg_write_at(vg_slot_offset(slot), &g_slots[slot],
                     sizeof(vg_safety_event_t));
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
  int fd;

  vg_reset_state();

  mkdir(base, 0755);
  snprintf(g_log_path, sizeof(g_log_path), "%s/events.bin", base);

  fd = vg_log_open(O_RDONLY);
  if (fd < 0)
    {
      /* 首次运行：创建文件并写入空头部 */

      vg_reset_state();
      if (vg_write_header() < 0)
        {
          /* 存储不可用时降级为纯内存日志，本地提醒能力不受影响 */

          fprintf(stderr,
                  "[velaguard] 事件日志文件不可用(%s)，降级为内存日志\n",
                  strerror(errno));
        }

      g_ready = true;
      return 0;
    }

  if (read(fd, &disk, sizeof(disk)) == (ssize_t)sizeof(disk) &&
      disk.magic == VG_LOG_MAGIC && disk.version == VG_LOG_VERSION &&
      disk.capacity == VG_EVENT_LOG_CAPACITY &&
      disk.head < VG_EVENT_LOG_CAPACITY &&
      disk.count <= VG_EVENT_LOG_CAPACITY)
    {
      g_hdr = disk;
      if (lseek(fd, vg_slot_offset(0), SEEK_SET) < 0 ||
          read(fd, g_slots, sizeof(g_slots)) < 0)
        {
          memset(g_slots, 0, sizeof(g_slots));
        }

      close(fd);
    }
  else
    {
      /* 新建或损坏：重建文件 */

      close(fd);
      if (truncate(g_log_path, 0) < 0)
        {
          fprintf(stderr, "[velaguard] 事件日志重建失败\n");
        }

      vg_reset_state();
      vg_write_header();
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
  uint32_t slot;

  if (!g_ready || evt == NULL || vg_event_validate(evt) != 0)
    {
      return -1;
    }

  slot = g_hdr.head;
  g_slots[slot] = *evt;

  g_hdr.head = (g_hdr.head + 1) % VG_EVENT_LOG_CAPACITY;
  if (g_hdr.count < VG_EVENT_LOG_CAPACITY)
    {
      g_hdr.count++;
    }

  g_hdr.total++;

  vg_write_slot(slot);
  vg_write_header();
  return 0;
}

int vg_event_log_update(const vg_safety_event_t *evt)
{
  uint32_t slot;

  if (!g_ready || evt == NULL)
    {
      return -1;
    }

  if (vg_find_slot(evt->event_id, &slot) < 0)
    {
      return -1;
    }

  g_slots[slot] = *evt;
  vg_write_slot(slot);
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
  vg_reset_state();

  if (g_log_path[0] != '\0' && truncate(g_log_path, 0) < 0)
    {
      fprintf(stderr, "[velaguard] 事件日志清空失败\n");
    }

  vg_write_header();
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
