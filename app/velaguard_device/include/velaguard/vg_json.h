/****************************************************************************
 * 安聆 VelaGuard - 极简 JSON 读写辅助
 *
 * 端侧只需要处理扁平的结构化事件摘要与配置文件，因此不引入完整 JSON 库，
 * 避免闪存增量与额外依赖（PRD-07 资源预算）。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_JSON_H
#define __VELAGUARD_VG_JSON_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 从扁平 JSON 文本中取出 "key" 对应的字符串值（自动反转义 \" \\ \n \t \uXXXX）。
 * 找到返回 0，未找到返回 -1。
 */

int vg_json_get_str(const char *json, const char *key,
                    char *out, size_t len);

/* 取数值。找到返回 0。 */

int vg_json_get_double(const char *json, const char *key, double *out);
int vg_json_get_int(const char *json, const char *key, int *out);
int vg_json_get_i64(const char *json, const char *key, long long *out);
int vg_json_get_bool(const char *json, const char *key, bool *out);

/* 向缓冲区追加一个转义后的 JSON 字符串（含引号）。
 * pos 为当前写入偏移，返回新的偏移；缓冲区不足时截断但保持合法结尾。
 */

size_t vg_json_put_escaped(char *buf, size_t len, size_t pos, const char *s);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_JSON_H */
