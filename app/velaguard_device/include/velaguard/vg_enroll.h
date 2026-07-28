/****************************************************************************
 * 安聆 VelaGuard - 个性化录入与模板匹配 (PRD-02)
 *
 * 只保存声学特征模板与预设显示名称，绝不保存原始录音；
 * 支持查看、重录、删除，删除立即生效且不可恢复。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_ENROLL_H
#define __VELAGUARD_VG_ENROLL_H

#include "velaguard/vg_feature.h"
#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define VG_TEMPLATE_MAX      16
#define VG_ENROLL_MIN_TAKES  3
#define VG_ENROLL_MAX_TAKES  5

/* 模板类别 */

typedef enum
{
  VG_TPL_NAME = 0,     /* 姓名 / 昵称 / 亲属称呼 */
  VG_TPL_HELP,         /* 明确求救短语 */
  VG_TPL_DIALECT,      /* 方言求救短语 */
  VG_TPL_KIND_MAX
} vg_tpl_kind_t;

/* 表达方式：同一短语需成对录制（PRD-02） */

typedef enum
{
  VG_TPL_CALM = 0,
  VG_TPL_URGENT
} vg_tpl_style_t;

typedef struct
{
  bool           used;
  vg_tpl_kind_t  kind;
  vg_tpl_style_t style;
  char           phrase[VG_PHRASE_LEN];   /* 预设短语标签 */
  char           person[VG_LABEL_LEN];    /* 预设人员标签 */
  uint8_t        takes;                   /* 已合并的采集次数 */
  float          centroid[VG_FEATURE_DIM];
  float          radius;                  /* 匹配半径 */
} vg_template_t;

typedef struct
{
  bool           hit;
  int            index;
  float          score;      /* 0~1，越大越像 */
  vg_tpl_kind_t  kind;
  vg_tpl_style_t style;
  char           phrase[VG_PHRASE_LEN];
  char           person[VG_LABEL_LEN];
} vg_enroll_match_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int  vg_enroll_init(const char *dir);
void vg_enroll_deinit(void);

/* 预设标签目录（板端无键盘，只能从预设中选择或自动编号） */

int         vg_enroll_preset_person_count(void);
const char *vg_enroll_preset_person(int idx);
int         vg_enroll_preset_phrase_count(vg_tpl_kind_t kind);
const char *vg_enroll_preset_phrase(vg_tpl_kind_t kind, int idx);

/* 录入流程：begin -> feed x(3~5) -> commit / cancel
 * 录入期间守护采集暂停（调用方负责 vg_capture_pause）。
 */

int  vg_enroll_begin(vg_tpl_kind_t kind, vg_tpl_style_t style,
                     const char *phrase, const char *person);
int  vg_enroll_feed(const float *feat);
int  vg_enroll_takes(void);
int  vg_enroll_commit(void);
void vg_enroll_cancel(void);
bool vg_enroll_active(void);

/* 管理 */

int  vg_enroll_count(void);
int  vg_enroll_get(int idx, vg_template_t *out);
int  vg_enroll_delete(int idx);
int  vg_enroll_clear(void);

/* 匹配：命中返回 0 并填充 out；未命中返回 -1。 */

int  vg_enroll_match(const float *feat, vg_enroll_match_t *out);

/* PRD-02 演示最小集自检：2 成员 + 2 姓名/称呼 + 3 求救短语 + 1 方言，
 * 且每条短语平静/急促成对。ready 返回 true，否则 why 说明缺什么。
 */

bool vg_enroll_min_set_ready(char *why, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_ENROLL_H */
