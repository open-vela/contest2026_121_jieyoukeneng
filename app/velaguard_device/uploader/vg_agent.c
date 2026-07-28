/****************************************************************************
 * 安聆 VelaGuard - anling-home-safety Skill 桥接实现 (PRD-06)
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "velaguard/vg_agent.h"
#include "velaguard/vg_json.h"
#include "velaguard/vg_notifier.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static char g_advice_json[4096];
static bool g_skill_ready;
static char g_source[32] = "内置模板";
static char g_skill_dir[128] = VG_SKILL_DIR;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int vg_load_advice_table(void)
{
  char path[192];
  FILE *fp;
  size_t n;

  snprintf(path, sizeof(path), "%s/advice.json", g_skill_dir);

  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return -1;
    }

  n = fread(g_advice_json, 1, sizeof(g_advice_json) - 1, fp);
  fclose(fp);

  if (n == 0)
    {
      return -1;
    }

  g_advice_json[n] = '\0';
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_agent_init(void)
{
  const char *env = getenv("VELAGUARD_SKILL_DIR");
  struct stat st;
  char path[192];

  if (env != NULL && env[0] != '\0')
    {
      vg_strlcpy(g_skill_dir, env, sizeof(g_skill_dir));
    }
  else
    {
      vg_strlcpy(g_skill_dir, VG_SKILL_DIR, sizeof(g_skill_dir));
    }

  snprintf(path, sizeof(path), "%s/SKILL.md", g_skill_dir);
  g_skill_ready = (stat(path, &st) == 0) && (vg_load_advice_table() == 0);

  if (g_skill_ready)
    {
      vg_strlcpy(g_source, "Skill 规则表", sizeof(g_source));
      printf("[velaguard] 已加载 anling-home-safety Skill: %s\n",
             g_skill_dir);
    }
  else
    {
      vg_strlcpy(g_source, "内置模板", sizeof(g_source));
      printf("[velaguard] 未部署 anling-home-safety Skill(%s)，"
             "通知使用端侧内置模板文案\n", g_skill_dir);
    }

  vg_notifier_set_advice_provider(vg_agent_advice);
  return g_skill_ready ? 0 : -1;
}

int vg_agent_advice(const vg_safety_event_t *evt, char *out, size_t len)
{
  char key[64];
  char action[160];

  if (evt == NULL || out == NULL || len == 0)
    {
      return -1;
    }

  /* --- 第 1 级：ai_agent / LLM ---------------------------------------
   * 板端 ai_agent 可用时，由它加载 VG_SKILL_DIR 下的 SKILL.md 并调用
   * `velaguard notify --json <事件摘要>` 把 LLM 生成的文案回灌给端侧
   * （见 SKILL.md「设备侧调用约定」）。端侧不主动阻塞等待 LLM，
   * 因此 ai_agent 不可用、断网或 LLM 失败都不会拖慢本地告警，
   * 直接落到第 2 级，通知不会丢失。
   */

  if (evt->summary[0] == '\0')
    {
      return -1;
    }

  /* --- 第 2 级：Skill 规则表 ------------------------------------------ */

  if (!g_skill_ready)
    {
      return -1;
    }

  /* 优先取「事件类型 + 等级」的专用文案，回退到事件类型通用文案 */

  snprintf(key, sizeof(key), "%s.%s",
           vg_event_type_str(evt->type), vg_level_str(evt->level));

  if (vg_json_get_str(g_advice_json, key, action, sizeof(action)) < 0 &&
      vg_json_get_str(g_advice_json, vg_event_type_str(evt->type),
                      action, sizeof(action)) < 0)
    {
      return -1;
    }

  snprintf(out, len, "[%s] %s。%s%s",
           vg_level_cn(evt->level), evt->summary, action,
           evt->time_reliable ? "" : "（设备时间未同步，时间仅供参考）");
  vg_utf8_trim(out);

  vg_strlcpy(g_source, "Skill 规则表", sizeof(g_source));
  return 0;
}

bool vg_agent_available(void)
{
  return g_skill_ready;
}

const char *vg_agent_source(void)
{
  return g_source;
}
