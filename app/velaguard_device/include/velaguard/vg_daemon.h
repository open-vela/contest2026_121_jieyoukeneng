/****************************************************************************
 * 安聆 VelaGuard - 守护主循环
 *
 * 把识别、状态机、UI、指示、上传串成一个周期任务；
 * 任何一环（网络/Agent/控制台/麦克风）不可用时，其余环节继续工作。
 ****************************************************************************/

#ifndef __VELAGUARD_VG_DAEMON_H
#define __VELAGUARD_VG_DAEMON_H

#include <stdbool.h>

#include "velaguard/vg_capture.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* 一次性初始化：配置、日志、状态机、识别、UI、上传、Skill */

int  vg_daemon_init(void);
void vg_daemon_deinit(void);

/* 启动/停止后台守护任务 */

int  vg_daemon_start(vg_source_t src, const char *path);
int  vg_daemon_stop(void);
bool vg_daemon_running(void);

/* 单步驱动一次主循环（供测试与单进程模式使用） */

void vg_daemon_step(void);

/* 状态摘要 */

void vg_daemon_status(void);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_DAEMON_H */
