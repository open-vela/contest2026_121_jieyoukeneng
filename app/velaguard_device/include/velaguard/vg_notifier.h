/****************************************************************************
 * 安聆 VelaGuard - 通知器 (PRD-06)
 *
 * notifier 输入 = 结构化事件 + Agent 文案，输出到可配置通道：
 *   主通道（V1 验收）: 局域网控制台 SSE -> 家属手机浏览器
 *   可选扩展        : webhook 转发（失败不影响主通道）
 *
 * LLM/Agent 不可用时使用本地模板文案兜底，绝不因 Agent 失败丢通知。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_NOTIFIER_H
#define __VELAGUARD_VG_NOTIFIER_H

#include "velaguard/vg_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define VG_ADVICE_LEN 256

/* Agent 文案提供者：成功返回 0 并填充 advice；失败返回负值触发模板兜底。 */

typedef int (*vg_advice_fn)(const vg_safety_event_t *evt,
                            char *advice, size_t len);

void vg_notifier_init(void);

/* 注册 Agent 文案提供者（anling-home-safety Skill 桥接） */

void vg_notifier_set_advice_provider(vg_advice_fn fn);

/* 生成通知文案：优先 Agent，失败自动回落到本地模板。
 * 返回 0 使用了 Agent 文案，1 使用了模板兜底。
 */

int vg_notifier_build_advice(const vg_safety_event_t *evt,
                             char *advice, size_t len);

/* 本地模板文案（永远可用，不依赖网络与 LLM） */

void vg_notifier_template(const vg_safety_event_t *evt,
                          char *advice, size_t len);

/* 按 UTF-8 边界修剪字符串尾部，避免截断出半个汉字 */

void vg_utf8_trim(char *s);

/* 组装通知负载：事件 JSON + advice 字段 */

int vg_notifier_build_payload(const vg_safety_event_t *evt,
                              const char *advice,
                              char *buf, size_t len);

/* 可选扩展通道：webhook 转发。失败返回负值，主通道不受影响。 */

int vg_notifier_webhook(const vg_safety_event_t *evt, const char *advice);

/* 统计：模板兜底次数 / Agent 成功次数 */

void vg_notifier_stats(uint32_t *agent_ok, uint32_t *template_fallback);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_NOTIFIER_H */
