/****************************************************************************
 * 安聆 VelaGuard - Wi-Fi 配置与状态接口
 ****************************************************************************/

#ifndef __VELAGUARD_VG_WIFI_H
#define __VELAGUARD_VG_WIFI_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define VG_WIFI_IFNAME       "wlan0"
#define VG_WIFI_MAX_NETWORKS 8
#define VG_WIFI_SSID_LEN     33
#define VG_WIFI_PASSWORD_LEN 64
#define VG_WIFI_IP_LEN       16
#define VG_WIFI_ERROR_LEN    64

typedef enum
{
  VG_WIFI_IDLE = 0,
  VG_WIFI_SCANNING,
  VG_WIFI_SCAN_READY,
  VG_WIFI_CONNECTING,
  VG_WIFI_ASSOCIATED,
  VG_WIFI_CONNECTED,
  VG_WIFI_FAILED
} vg_wifi_state_t;

typedef struct
{
  char ssid[VG_WIFI_SSID_LEN];
  int  rssi;
  bool secured;
} vg_wifi_network_t;

typedef struct
{
  vg_wifi_state_t state;
  unsigned int    network_count;
  unsigned int    selected;
  bool            ip_ready;
  char            ssid[VG_WIFI_SSID_LEN];
  char            ip[VG_WIFI_IP_LEN];
  char            error[VG_WIFI_ERROR_LEN];
} vg_wifi_status_t;

int  vg_wifi_init(void);
void vg_wifi_deinit(void);
int  vg_wifi_start_scan(void);
int  vg_wifi_select(int delta);
int  vg_wifi_get_network(unsigned int index, vg_wifi_network_t *network);
int  vg_wifi_get_status(vg_wifi_status_t *status);
int  vg_wifi_connect(const char *ssid, const char *password, bool save);
int  vg_wifi_connect_selected(const char *password, bool save);
const char *vg_wifi_state_text(vg_wifi_state_t state, bool ascii);

#ifdef __cplusplus
}
#endif

#endif /* __VELAGUARD_VG_WIFI_H */
