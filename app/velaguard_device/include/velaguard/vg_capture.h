/****************************************************************************
 * 安聆 VelaGuard - 音频采集抽象 (PRD-01)
 *
 * 两种音频源：
 *   VG_SRC_MIC  实机线：板载麦克风 PCM 设备（16kHz 单声道）
 *   VG_SRC_WAV  模拟器线：读取 wav 文件，走与实机完全相同的特征与推理链路
 *
 * 守护采集独占麦克风，与 ai_agent PTT 对话、个性化录入互斥；
 * 互斥期间调用 vg_capture_pause()，UI 显示"守护已暂停"，退出后自动恢复。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_CAPTURE_H
#define __VELAGUARD_VG_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
  VG_SRC_NONE = 0,
  VG_SRC_MIC,
  VG_SRC_WAV
} vg_source_t;

/* 打开音频源。VG_SRC_WAV 时 path 为 wav 路径；VG_SRC_MIC 时 path 为 NULL
 * 表示使用编译期配置的 PCM 设备。成功返回 0。
 */

int vg_capture_open(vg_source_t src, const char *path);

/* 读取 nsamples 个 16kHz 单声道样本。
 * 返回实际读取数；0 表示 wav 播放结束；负值表示错误。
 * 采集暂停期间返回 0 并不推进。
 */

int vg_capture_read(int16_t *buf, size_t nsamples);

void vg_capture_close(void);

vg_source_t vg_capture_source(void);
const char *vg_capture_source_name(void);

/* 音频通路互斥（PRD-01/04） */

void        vg_capture_pause(const char *reason);
void        vg_capture_resume(void);
bool        vg_capture_paused(void);
const char *vg_capture_pause_reason(void);

/* 提示音播放期间的时分让路：不支持全双工时，播放前调用 hold，
 * 播放结束调用 release，单次中断上限 2 秒（PRD-04）。
 */

void vg_capture_hold_for_playback(uint32_t max_ms);
void vg_capture_release_playback(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_CAPTURE_H */
