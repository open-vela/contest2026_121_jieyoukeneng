/****************************************************************************
 * 安聆 VelaGuard - anling-home-safety Skill 桥接 (PRD-06)
 *
 * Skill 只接收结构化事件（类型、时间、持续、置信度、是否响应），
 * 绝不接收原始音频或对话文本。
 *
 * 文案生成有三级回退，保证"不因 Agent 失败丢通知"：
 *   1. ai_agent 在线：由 LLM 按 Skill 指令生成个性化文案（需板端 ai_agent 可用）
 *   2. Skill 规则表：读取 /data/agent/skills/anling-home-safety/advice.json
 *   3. 端侧内置模板：vg_notifier_template()
 ****************************************************************************/

#ifndef __VELAGUARD_VG_AGENT_H
#define __VELAGUARD_VG_AGENT_H

#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define VG_SKILL_DIR "/data/agent/skills/anling-home-safety"

/* 注册到 notifier；返回 0 表示 Skill 可用 */

int vg_agent_init(void);

/* 文案提供者：成功返回 0，失败返回负值（由 notifier 回落到内置模板） */

int vg_agent_advice(const vg_safety_event_t *evt, char *out, size_t len);

/* Skill 是否已部署（供 status 显示） */

bool        vg_agent_available(void);
const char *vg_agent_source(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_AGENT_H */
