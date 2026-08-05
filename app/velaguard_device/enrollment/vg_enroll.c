/****************************************************************************
 * 安聆 VelaGuard - 个性化录入与模板匹配实现 (PRD-02)
 *
 * 模板 = 3~5 次采集特征的质心 + 匹配半径；匹配用归一化欧氏距离。
 * 该方案是 PRD-07 "通用关键词模型不稳定时的保底路线"，无需任何推理框架。
 ****************************************************************************/

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "velaguard/vg_config.h"
#include "velaguard/vg_enroll.h"
#include "velaguard/vg_model.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VG_TPL_MAGIC   0x56475431u   /* "VGT1" */

/* 匹配判定：距离小于半径即命中，score 由距离线性映射 */

#define VG_MATCH_MARGIN 1.25f
#define VG_PERSON_SEEN_MAX 16 /* >= g_preset_person 元素数，预留扩展空间 */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_preset_person[] =
{
  "家庭成员 A", "家庭成员 B", "家庭成员 C", "老伴", "儿子", "女儿"
};

static const char * const g_preset_name[] =
{
  "姓名/昵称", "老伴", "妈妈", "爸爸", "奶奶", "爷爷"
};

static const char * const g_preset_help[] =
{
  "救命", "快来人", "来人啊", "我摔倒了", "帮帮我"
};

static const char * const g_preset_dialect[] =
{
  "救命啊(方言)", "来人咯(方言)", "疼死了(方言)"
};

static vg_template_t g_templates[VG_TEMPLATE_MAX];
static char          g_tpl_path[VG_PATH_LEN + 32];

/* 录入会话状态 */

static bool           g_enroll_active;
static vg_template_t  g_pending;
static float          g_takes[VG_ENROLL_MAX_TAKES][VG_FEATURE_DIM];
static int            g_take_count;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 用环境模型的归一化参数把特征拉到同一尺度，避免个别维度主导距离 */

static void vg_normalize(const float *feat, float *out)
{
  int i;

  for (i = 0; i < VG_FEATURE_DIM; i++)
    {
      float s = (g_vg_env_scale[i] > 1e-6f) ? g_vg_env_scale[i] : 1.0f;

      out[i] = (feat[i] - g_vg_env_mean[i]) / s;
    }
}

static float vg_distance(const float *a, const float *b)
{
  float acc = 0.0f;
  int i;

  for (i = 0; i < VG_FEATURE_DIM; i++)
    {
      float d = a[i] - b[i];

      acc += d * d;
    }

  return sqrtf(acc / VG_FEATURE_DIM);
}

static int vg_tpl_save(void)
{
  uint32_t magic = VG_TPL_MAGIC;
  int fd;

  if (g_tpl_path[0] == '\0')
    {
      return -1;
    }

  fd = open(g_tpl_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      return -1;
    }

  if (write(fd, &magic, sizeof(magic)) != (ssize_t)sizeof(magic) ||
      write(fd, g_templates, sizeof(g_templates)) !=
      (ssize_t)sizeof(g_templates))
    {
      close(fd);
      return -1;
    }

  fsync(fd);
  close(fd);
  return 0;
}

static int vg_tpl_load(void)
{
  uint32_t magic = 0;
  int fd;

  fd = open(g_tpl_path, O_RDONLY);
  if (fd < 0)
    {
      return -1;
    }

  if (read(fd, &magic, sizeof(magic)) != (ssize_t)sizeof(magic) ||
      magic != VG_TPL_MAGIC ||
      read(fd, g_templates, sizeof(g_templates)) !=
      (ssize_t)sizeof(g_templates))
    {
      memset(g_templates, 0, sizeof(g_templates));
      close(fd);
      return -1;
    }

  close(fd);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_enroll_init(const char *dir)
{
  const char *base = (dir != NULL) ? dir : vg_config()->data_dir;

  memset(g_templates, 0, sizeof(g_templates));
  g_enroll_active = false;
  g_take_count = 0;

  mkdir(base, 0755);
  snprintf(g_tpl_path, sizeof(g_tpl_path), "%s/templates.bin", base);
  vg_tpl_load();
  return 0;
}

void vg_enroll_deinit(void)
{
  g_enroll_active = false;
}

int vg_enroll_preset_person_count(void)
{
  return (int)(sizeof(g_preset_person) / sizeof(g_preset_person[0]));
}

const char *vg_enroll_preset_person(int idx)
{
  if (idx < 0 || idx >= vg_enroll_preset_person_count())
    {
      return "";
    }

  return g_preset_person[idx];
}

int vg_enroll_preset_phrase_count(vg_tpl_kind_t kind)
{
  switch (kind)
    {
      case VG_TPL_NAME:
        return (int)(sizeof(g_preset_name) / sizeof(g_preset_name[0]));

      case VG_TPL_HELP:
        return (int)(sizeof(g_preset_help) / sizeof(g_preset_help[0]));

      case VG_TPL_DIALECT:
        return (int)(sizeof(g_preset_dialect) / sizeof(g_preset_dialect[0]));

      default:
        return 0;
    }
}

const char *vg_enroll_preset_phrase(vg_tpl_kind_t kind, int idx)
{
  if (idx < 0 || idx >= vg_enroll_preset_phrase_count(kind))
    {
      return "";
    }

  switch (kind)
    {
      case VG_TPL_NAME:
        return g_preset_name[idx];

      case VG_TPL_HELP:
        return g_preset_help[idx];

      default:
        return g_preset_dialect[idx];
    }
}

int vg_enroll_begin(vg_tpl_kind_t kind, vg_tpl_style_t style,
                    const char *phrase, const char *person)
{
  if (kind < 0 || kind >= VG_TPL_KIND_MAX)
    {
      return -1;
    }

  memset(&g_pending, 0, sizeof(g_pending));
  g_pending.kind = kind;
  g_pending.style = style;

  vg_strlcpy(g_pending.phrase,
          (phrase != NULL && phrase[0] != '\0') ?
          phrase : vg_enroll_preset_phrase(kind, 0), sizeof(g_pending.phrase));
  vg_strlcpy(g_pending.person,
          (person != NULL && person[0] != '\0') ?
          person : vg_enroll_preset_person(0), sizeof(g_pending.person));

  g_take_count = 0;
  g_enroll_active = true;
  return 0;
}

int vg_enroll_feed(const float *feat)
{
  if (!g_enroll_active || feat == NULL)
    {
      return -1;
    }

  if (g_take_count >= VG_ENROLL_MAX_TAKES)
    {
      return -2;
    }

  vg_normalize(feat, g_takes[g_take_count]);
  g_take_count++;
  return g_take_count;
}

int vg_enroll_takes(void)
{
  return g_take_count;
}

bool vg_enroll_active(void)
{
  return g_enroll_active;
}

int vg_enroll_commit(void)
{
  float maxd = 0.0f;
  int slot = -1;
  int i;
  int j;

  if (!g_enroll_active)
    {
      return -1;
    }

  if (g_take_count < VG_ENROLL_MIN_TAKES)
    {
      return -2;
    }

  /* 同一 (kind, phrase, person, style) 视为重录，直接覆盖 */

  for (i = 0; i < VG_TEMPLATE_MAX; i++)
    {
      if (g_templates[i].used &&
          g_templates[i].kind == g_pending.kind &&
          g_templates[i].style == g_pending.style &&
          strcmp(g_templates[i].phrase, g_pending.phrase) == 0 &&
          strcmp(g_templates[i].person, g_pending.person) == 0)
        {
          slot = i;
          break;
        }
    }

  if (slot < 0)
    {
      for (i = 0; i < VG_TEMPLATE_MAX; i++)
        {
          if (!g_templates[i].used)
            {
              slot = i;
              break;
            }
        }
    }

  if (slot < 0)
    {
      g_enroll_active = false;
      return -3;
    }

  /* 质心 */

  for (j = 0; j < VG_FEATURE_DIM; j++)
    {
      float sum = 0.0f;

      for (i = 0; i < g_take_count; i++)
        {
          sum += g_takes[i][j];
        }

      g_pending.centroid[j] = sum / g_take_count;
    }

  /* 半径：采集样本到质心的最大距离，留出裕度 */

  for (i = 0; i < g_take_count; i++)
    {
      float d = vg_distance(g_takes[i], g_pending.centroid);

      if (d > maxd)
        {
          maxd = d;
        }
    }

  g_pending.radius = (maxd > 1e-4f) ? maxd * VG_MATCH_MARGIN : 0.35f;
  g_pending.takes = (uint8_t)g_take_count;
  g_pending.used = true;

  g_templates[slot] = g_pending;
  g_enroll_active = false;
  g_take_count = 0;

  vg_tpl_save();
  return slot;
}

void vg_enroll_cancel(void)
{
  g_enroll_active = false;
  g_take_count = 0;
  memset(&g_pending, 0, sizeof(g_pending));
}

int vg_enroll_count(void)
{
  int n = 0;
  int i;

  for (i = 0; i < VG_TEMPLATE_MAX; i++)
    {
      if (g_templates[i].used)
        {
          n++;
        }
    }

  return n;
}

int vg_enroll_get(int idx, vg_template_t *out)
{
  int n = 0;
  int i;

  if (out == NULL || idx < 0)
    {
      return -1;
    }

  for (i = 0; i < VG_TEMPLATE_MAX; i++)
    {
      if (g_templates[i].used && n++ == idx)
        {
          *out = g_templates[i];
          return i;
        }
    }

  return -1;
}

int vg_enroll_delete(int idx)
{
  vg_template_t tmp;
  int slot = vg_enroll_get(idx, &tmp);

  if (slot < 0)
    {
      return -1;
    }

  /* 立即生效且不可恢复：清零后落盘 */

  memset(&g_templates[slot], 0, sizeof(g_templates[slot]));
  vg_tpl_save();
  return 0;
}

int vg_enroll_clear(void)
{
  memset(g_templates, 0, sizeof(g_templates));
  vg_tpl_save();
  return 0;
}

int vg_enroll_match(const float *feat, vg_enroll_match_t *out)
{
  float norm[VG_FEATURE_DIM];
  float best_score = 0.0f;
  int best = -1;
  int i;

  if (feat == NULL || out == NULL)
    {
      return -1;
    }

  memset(out, 0, sizeof(*out));
  vg_normalize(feat, norm);

  for (i = 0; i < VG_TEMPLATE_MAX; i++)
    {
      float d;
      float score;

      if (!g_templates[i].used)
        {
          continue;
        }

      d = vg_distance(norm, g_templates[i].centroid);
      if (d >= g_templates[i].radius)
        {
          continue;
        }

      score = 1.0f - d / g_templates[i].radius;
      if (score > best_score)
        {
          best_score = score;
          best = i;
        }
    }

  if (best < 0)
    {
      return -1;
    }

  out->hit = true;
  out->index = best;
  out->score = best_score;
  out->kind = g_templates[best].kind;
  out->style = g_templates[best].style;
  vg_strlcpy(out->phrase, g_templates[best].phrase, sizeof(out->phrase));
  vg_strlcpy(out->person, g_templates[best].person, sizeof(out->person));
  return 0;
}

bool vg_enroll_min_set_ready(char *why, size_t len)
{
  bool person_seen[VG_PERSON_SEEN_MAX] = { false };
  int persons = 0;
  int names = 0;
  int helps = 0;
  int dialects = 0;
  int unpaired = 0;
  int i;
  int j;

  for (i = 0; i < VG_TEMPLATE_MAX; i++)
    {
      const vg_template_t *t = &g_templates[i];
      bool paired = false;

      if (!t->used)
        {
          continue;
        }

      for (j = 0; j < vg_enroll_preset_person_count() && j < VG_PERSON_SEEN_MAX; j++)
        {
          if (strcmp(t->person, vg_enroll_preset_person(j)) == 0 &&
              !person_seen[j])
            {
              person_seen[j] = true;
              persons++;
              break;
            }
        }

      switch (t->kind)
        {
          case VG_TPL_NAME:
            names++;
            break;

          case VG_TPL_HELP:
            helps++;
            break;

          default:
            dialects++;
            break;
        }

      /* 平静/急促成对检查 */

      for (j = 0; j < VG_TEMPLATE_MAX; j++)
        {
          if (j != i && g_templates[j].used &&
              g_templates[j].kind == t->kind &&
              g_templates[j].style != t->style &&
              strcmp(g_templates[j].phrase, t->phrase) == 0)
            {
              paired = true;
              break;
            }
        }

      if (!paired)
        {
          unpaired++;
        }
    }

  /* 成对统计时每条短语被计两次，这里按条目数折算 */

  if (persons >= 2 && names >= 2 && helps >= 3 && dialects >= 1 &&
      unpaired == 0)
    {
      if (why != NULL && len > 0)
        {
          snprintf(why, len, "录入最小集已满足（%d 条模板）",
                   vg_enroll_count());
        }

      return true;
    }

  if (why != NULL && len > 0)
    {
      snprintf(why, len,
               "缺: 成员%d/2 称呼%d/2 求救词%d/3 方言%d/1 未成对%d",
               persons, names, helps, dialects, unpaired);
    }

  return false;
}
