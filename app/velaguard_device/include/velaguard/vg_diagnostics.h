/****************************************************************************
 * 安聆 VelaGuard - 板端诊断入口
 ****************************************************************************/

#ifndef __VELAGUARD_VG_DIAGNOSTICS_H
#define __VELAGUARD_VG_DIAGNOSTICS_H

#include <stdbool.h>

typedef enum
{
  VG_DIAG_MIC = 0,
  VG_DIAG_SPEAKER,
  VG_DIAG_NETWORK,
  VG_DIAG_ALL
} vg_diag_kind_t;

int vg_diagnostics_run(vg_diag_kind_t kind);
int vg_diagnostics_start(vg_diag_kind_t kind);
bool vg_diagnostics_running(void);

#endif
