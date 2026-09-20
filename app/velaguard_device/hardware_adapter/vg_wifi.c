/****************************************************************************
 * 安聆 VelaGuard - Wi-Fi 配置与状态实现
 *
 * WAPI 的扫描、关联和配置调用都放在后台线程中。LVGL 线程只读取一份
 * 受 mutex 保护的快照，避免扫描或 DHCP 阻塞触摸事件和屏幕刷新。
 ****************************************************************************/

#ifdef __NuttX__
#  include <nuttx/config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_WIRELESS_WAPI
#  include <arpa/inet.h>
#  include <netinet/ether.h>
#  include <netutils/netlib.h>
#  include <wireless/wapi.h>
#endif

#include "velaguard/vg_wifi.h"

#ifndef CONFIG_VELAGUARD_WIFI_STACKSIZE
#  define CONFIG_VELAGUARD_WIFI_STACKSIZE 204800
#endif

#ifndef CONFIG_VELAGUARD_PRIORITY
#  define CONFIG_VELAGUARD_PRIORITY 100
#endif

#define VG_WIFI_DEINIT_TIMEOUT_MS 3000

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t   g_wifi_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_wifi_worker_cond = PTHREAD_COND_INITIALIZER;
static vg_wifi_status_t  g_wifi_status;
static vg_wifi_network_t g_wifi_networks[VG_WIFI_MAX_NETWORKS];
#ifndef __NuttX__
static pthread_t          g_wifi_worker;
#endif
static bool               g_wifi_worker_running;
static bool               g_wifi_worker_exited = true;
#ifndef __NuttX__
static bool               g_wifi_worker_valid;
static bool               g_wifi_worker_reaping;
#endif
static bool               g_wifi_stop_requested;
static bool               g_wifi_stopping;
static bool               g_wifi_ready;
#ifdef CONFIG_WIRELESS_WAPI
static char               g_pending_ssid[VG_WIFI_SSID_LEN];
static char               g_pending_password[VG_WIFI_PASSWORD_LEN];
static bool               g_pending_save;

typedef enum
{
  VG_WIFI_WORKER_SCAN = 0,
  VG_WIFI_WORKER_CONNECT
} vg_wifi_worker_kind_t;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void vg_wifi_set_error(const char *message)
{
  pthread_mutex_lock(&g_wifi_lock);
  g_wifi_status.state = VG_WIFI_FAILED;
  g_wifi_status.error[0] = '\0';
  if (message != NULL)
    {
      snprintf(g_wifi_status.error, sizeof(g_wifi_status.error), "%s",
               message);
    }
  pthread_mutex_unlock(&g_wifi_lock);
}

#ifdef CONFIG_WIRELESS_WAPI
static void vg_wifi_set_notice(const char *message)
{
  pthread_mutex_lock(&g_wifi_lock);
  if (message != NULL)
    {
      snprintf(g_wifi_status.error, sizeof(g_wifi_status.error), "%s",
               message);
    }
  pthread_mutex_unlock(&g_wifi_lock);
}
#endif

#ifdef CONFIG_WIRELESS_WAPI
static void vg_wifi_worker_done(void)
{
  pthread_mutex_lock(&g_wifi_lock);
  g_wifi_worker_running = false;
  pthread_cond_broadcast(&g_wifi_worker_cond);
  pthread_mutex_unlock(&g_wifi_lock);
}

static void vg_wifi_worker_exit(void)
{
  pthread_mutex_lock(&g_wifi_lock);
  g_wifi_stopping = false;
  g_wifi_worker_exited = true;
  g_wifi_status.operation_busy = false;
  if (!g_wifi_ready)
    {
      g_wifi_status.state = VG_WIFI_IDLE;
      g_wifi_status.network_count = 0;
      g_wifi_status.selected = 0;
      g_wifi_status.ssid[0] = '\0';
      g_wifi_status.ip_ready = false;
      g_wifi_status.ip[0] = '\0';
      g_wifi_status.error[0] = '\0';
    }
  pthread_cond_broadcast(&g_wifi_worker_cond);
  pthread_mutex_unlock(&g_wifi_lock);
}

static bool vg_wifi_worker_should_stop(void)
{
  bool stop;

  pthread_mutex_lock(&g_wifi_lock);
  stop = g_wifi_stop_requested || !g_wifi_ready;
  pthread_mutex_unlock(&g_wifi_lock);
  return stop;
}
#endif

static int vg_wifi_reap_worker(void)
{
#ifdef __NuttX__
  /* NuttX 使用分离式 pthread，线程退出时由系统自动回收资源。 */
  return 0;
#else
  pthread_t thread;
  int ret = 0;
  bool reap = false;

  pthread_mutex_lock(&g_wifi_lock);
  if (g_wifi_worker_valid && !g_wifi_worker_running)
    {
      if (g_wifi_worker_reaping)
        {
          pthread_mutex_unlock(&g_wifi_lock);
          return -EBUSY;
        }

      thread = g_wifi_worker;
      g_wifi_worker_reaping = true;
      reap = true;
    }
  pthread_mutex_unlock(&g_wifi_lock);

  if (!reap)
    {
      return 0;
    }

  ret = pthread_join(thread, NULL);
  if (ret > 0)
    {
      ret = -ret;
    }

  pthread_mutex_lock(&g_wifi_lock);
  if (ret == 0)
    {
      g_wifi_worker_valid = false;
    }
  g_wifi_worker_reaping = false;
  pthread_cond_broadcast(&g_wifi_worker_cond);
  pthread_mutex_unlock(&g_wifi_lock);
  return ret;
#endif
}

#ifdef CONFIG_WIRELESS_WAPI
static void vg_wifi_set_ip_status(int sock, bool *ip_ready)
{
  struct in_addr addr;
  const char *text;

  /* WAPI 的地址缓存可能滞后于网卡实际状态；DHCP 完成后直接读取
   * netlib 的接口地址，避免旧地址把连接误判为可用。 */
  (void)sock;
  *ip_ready = false;
  memset(&addr, 0, sizeof(addr));
  if (netlib_get_ipv4addr(VG_WIFI_IFNAME, &addr) < 0 || addr.s_addr == 0)
    {
      return;
    }

  text = inet_ntoa(addr);
  if (text == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_wifi_lock);
  snprintf(g_wifi_status.ip, sizeof(g_wifi_status.ip), "%s", text);
  g_wifi_status.ip_ready = true;
  pthread_mutex_unlock(&g_wifi_lock);
  *ip_ready = true;
}

static int vg_wifi_collect_scan(int sock)
{
  struct wapi_list_s list;
  struct wapi_scan_info_s *info;
  vg_wifi_network_t found[VG_WIFI_MAX_NETWORKS];
  unsigned int count = 0;
  int ret;

  memset(&list, 0, sizeof(list));
  memset(found, 0, sizeof(found));
  ret = wapi_scan_coll(sock, VG_WIFI_IFNAME, &list);
  if (ret < 0)
    {
      wapi_scan_coll_free(&list);
      return ret;
    }

  for (info = list.head.scan; info != NULL && count < VG_WIFI_MAX_NETWORKS;
       info = info->next)
    {
      unsigned int i;
      bool duplicate = false;

      if (!info->has_essid || info->essid[0] == '\0')
        {
          continue;
        }

      for (i = 0; i < count; i++)
        {
          if (strcmp(found[i].ssid, info->essid) == 0)
            {
              duplicate = true;
              break;
            }
        }

      if (!duplicate)
        {
          snprintf(found[count].ssid, sizeof(found[count].ssid), "%s",
                   info->essid);
          found[count].rssi = info->has_rssi ? info->rssi : 0;
          found[count].secured = info->has_encode && info->encode != 0;
          count++;
        }
    }

  wapi_scan_coll_free(&list);
  pthread_mutex_lock(&g_wifi_lock);
  if (g_wifi_stop_requested || !g_wifi_ready)
    {
      pthread_mutex_unlock(&g_wifi_lock);
      return -ECANCELED;
    }
  memcpy(g_wifi_networks, found, sizeof(found));
  g_wifi_status.network_count = count;
  g_wifi_status.selected = 0;
  if (count > 0)
    {
      snprintf(g_wifi_status.ssid, sizeof(g_wifi_status.ssid), "%s",
               found[0].ssid);
    }
  else
    {
      g_wifi_status.ssid[0] = '\0';
    }
  g_wifi_status.state = VG_WIFI_SCAN_READY;
  g_wifi_status.error[0] = '\0';
  pthread_mutex_unlock(&g_wifi_lock);
  return 0;
}

static void *vg_wifi_scan_worker(void *arg)
{
  int sock;
  int ret;
  int tries;
  bool cancelled = false;

  (void)arg;
  sock = wapi_make_socket();
  if (sock < 0)
    {
      vg_wifi_set_error("无法打开 Wi-Fi 控制接口");
      vg_wifi_worker_done();
      return NULL;
    }

  ret = wapi_set_ifup(sock, VG_WIFI_IFNAME);
  if (ret < 0)
    {
      close(sock);
      vg_wifi_set_error("Wi-Fi 网卡未启动");
      vg_wifi_worker_done();
      return NULL;
    }

  if (vg_wifi_worker_should_stop())
    {
      close(sock);
      vg_wifi_set_error("Wi-Fi 操作已取消");
      vg_wifi_worker_done();
      return NULL;
    }

  ret = wapi_scan_init(sock, VG_WIFI_IFNAME, NULL);
  if (ret < 0)
    {
      close(sock);
      vg_wifi_set_error("扫描启动失败");
      vg_wifi_worker_done();
      return NULL;
    }

  for (tries = 0; tries < 40; tries++)
    {
      if (vg_wifi_worker_should_stop())
        {
          cancelled = true;
          break;
        }

      ret = wapi_scan_stat(sock, VG_WIFI_IFNAME);
      if (ret <= 0)
        {
          break;
        }

      usleep(250000);
    }

  if (cancelled)
    {
      close(sock);
      vg_wifi_set_error("Wi-Fi 操作已取消");
      vg_wifi_worker_done();
      return NULL;
    }

  if (ret != 0)
    {
      close(sock);
      vg_wifi_set_error("扫描超时，请重试");
      vg_wifi_worker_done();
      return NULL;
    }

  ret = vg_wifi_collect_scan(sock);
  close(sock);
  if (ret < 0 && ret != -ECANCELED)
    {
      vg_wifi_set_error("读取扫描结果失败");
    }

  vg_wifi_worker_done();
  return NULL;
}

static int vg_wifi_save_config(int sock, const char *ssid,
                               const char *password)
{
#ifndef CONFIG_WIRELESS_WAPI_INITCONF
  (void)sock;
  (void)ssid;
  (void)password;
  return -ENOTSUP;
#else
  struct wpa_wconfig_s config;
  char bssid[18];
  struct ether_addr ap;

  if (wapi_get_ap(sock, VG_WIFI_IFNAME, &ap) == 0)
    {
      snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
               ap.ether_addr_octet[0], ap.ether_addr_octet[1],
               ap.ether_addr_octet[2], ap.ether_addr_octet[3],
               ap.ether_addr_octet[4], ap.ether_addr_octet[5]);
    }
  else
    {
      /* WAPI 配置加载要求 bssid 字段存在且可解析；全零地址表示不锁定
       * AP，后续仍由 ESSID 选择实际热点。 */
      snprintf(bssid, sizeof(bssid), "00:00:00:00:00:00");
    }

  memset(&config, 0, sizeof(config));
  config.sta_mode = WAPI_MODE_MANAGED;
  config.auth_wpa = password[0] == '\0' ? IW_AUTH_WPA_VERSION_DISABLED
                                        : IW_AUTH_WPA_VERSION_WPA2;
  config.cipher_mode = password[0] == '\0' ? IW_AUTH_CIPHER_NONE
                                            : IW_AUTH_CIPHER_CCMP;
  config.alg = password[0] == '\0' ? WPA_ALG_NONE : WPA_ALG_CCMP;
  config.ssidlen = (uint8_t)strlen(ssid);
  config.phraselen = (uint8_t)strlen(password);
  config.ifname = VG_WIFI_IFNAME;
  config.ssid = ssid;
  config.bssid = bssid;
  config.passphrase = password;
  return wapi_save_config(VG_WIFI_IFNAME, NULL, &config);
#endif
}

static int vg_wifi_configure_security(int sock, const char *password)
{
  int ret;

  if (password[0] == '\0')
    {
      ret = wpa_driver_wext_set_auth_param(sock, VG_WIFI_IFNAME,
                                           IW_AUTH_WPA_VERSION,
                                           IW_AUTH_WPA_VERSION_DISABLED);
      if (ret == 0)
        {
          ret = wpa_driver_wext_set_auth_param(sock, VG_WIFI_IFNAME,
                                               IW_AUTH_CIPHER_PAIRWISE,
                                               IW_AUTH_CIPHER_NONE);
        }
    }
  else
    {
      ret = wpa_driver_wext_set_auth_param(sock, VG_WIFI_IFNAME,
                                           IW_AUTH_WPA_VERSION,
                                           IW_AUTH_WPA_VERSION_WPA2);
      if (ret == 0)
        {
          ret = wpa_driver_wext_set_auth_param(sock, VG_WIFI_IFNAME,
                                               IW_AUTH_CIPHER_PAIRWISE,
                                               IW_AUTH_CIPHER_CCMP);
        }
      if (ret == 0)
        {
          ret = wpa_driver_wext_set_key_ext(sock, VG_WIFI_IFNAME,
                                            WPA_ALG_CCMP, password,
                                            strlen(password));
        }
    }

  return ret;
}

static void *vg_wifi_connect_worker(void *arg)
{
  char ssid[VG_WIFI_SSID_LEN];
  char password[sizeof(g_pending_password)];
  bool save;
  bool ip_ready = false;
  bool associated = false;
  bool cancelled = false;
  bool dhcp_reset_failed = false;
  bool save_failed = false;
  int dhcp_attempts = 0;
  int last_dhcp_try = -5;
  enum wapi_essid_flag_e flag = WAPI_ESSID_OFF;
  char current[VG_WIFI_SSID_LEN];
  int sock;
  int ret;
  int tries;

  (void)arg;
  pthread_mutex_lock(&g_wifi_lock);
  snprintf(ssid, sizeof(ssid), "%s", g_pending_ssid);
  snprintf(password, sizeof(password), "%s", g_pending_password);
  save = g_pending_save;
  memset(g_pending_password, 0, sizeof(g_pending_password));
  pthread_mutex_unlock(&g_wifi_lock);

  sock = wapi_make_socket();
  if (sock < 0)
    {
      vg_wifi_set_error("无法打开 Wi-Fi 控制接口");
      memset(password, 0, sizeof(password));
      vg_wifi_worker_done();
      return NULL;
    }

  wpa_driver_wext_disconnect(sock, VG_WIFI_IFNAME);
  ret = wapi_set_ifup(sock, VG_WIFI_IFNAME);
  if (ret == 0)
    {
      ret = wapi_set_mode(sock, VG_WIFI_IFNAME, WAPI_MODE_MANAGED);
    }
  if (ret == 0)
    {
      ret = vg_wifi_configure_security(sock, password);
    }
  if (ret == 0)
    {
      /* 与开发板 start_wifi.sh 一致：关闭省电，避免关联后 DHCP 被拖慢。 */
      (void)wapi_set_power_save(sock, VG_WIFI_IFNAME, false);
    }
  if (ret == 0)
    {
      ret = wapi_set_essid(sock, VG_WIFI_IFNAME, ssid, WAPI_ESSID_ON);
    }
  if (ret < 0)
    {
      close(sock);
      vg_wifi_set_error("Wi-Fi 参数设置失败");
      memset(password, 0, sizeof(password));
      vg_wifi_worker_done();
      return NULL;
    }

  for (tries = 0; tries < 40; tries++)
    {
      if (vg_wifi_worker_should_stop())
        {
          cancelled = true;
          break;
        }

      associated = false;
      memset(current, 0, sizeof(current));
      if (wapi_get_essid(sock, VG_WIFI_IFNAME, current, &flag) == 0 &&
          flag == WAPI_ESSID_ON && strcmp(current, ssid) == 0)
        {
          associated = true;
          if (!ip_ready && dhcp_attempts < 3 &&
              tries - last_dhcp_try >= 5)
            {
              /* 只有确认已经关联后再请求 DHCP，避免过早请求导致
               * 后续虽然关联成功却一直没有地址。 */

              struct in_addr zero_addr;
              int dhcp_ret;

              memset(&zero_addr, 0, sizeof(zero_addr));
              if (dhcp_attempts == 0)
                {
                  if (netlib_set_ipv4addr(VG_WIFI_IFNAME, &zero_addr) < 0)
                    {
                      dhcp_reset_failed = true;
                      dhcp_attempts = 2;
                    }
                }
              if (!dhcp_reset_failed)
                {
                  dhcp_attempts++;
                  last_dhcp_try = tries;
                  dhcp_ret = netlib_obtain_ipv4addr(VG_WIFI_IFNAME);
                  if (dhcp_ret == 0)
                    {
                      vg_wifi_set_ip_status(sock, &ip_ready);
                    }
                }
            }
          if (ip_ready)
            {
              break;
            }
        }
      usleep(250000);
    }

  /* DHCP 可能在停止请求到来时仍阻塞；返回后必须再次检查，避免
   * deinit 已经开始却继续保存凭据或发布“已连接”状态。 */
  if (!cancelled && vg_wifi_worker_should_stop())
    {
      cancelled = true;
    }

  {
    bool save_requested = false;

    pthread_mutex_lock(&g_wifi_lock);
    if (!cancelled && (g_wifi_stop_requested || !g_wifi_ready))
      {
        cancelled = true;
      }
    save_requested = !cancelled && associated && save;
    pthread_mutex_unlock(&g_wifi_lock);

    /* 配置落盘可能被文件系统或 fsync 拖慢，不能占着全局锁阻塞
     * deinit 设置停止标志。落盘完成后再次检查停止状态，停止中的
     * 连接不会发布为成功；如果写入已经开始，则保留原子写结果。
     */
    if (save_requested)
      {
        save_failed = vg_wifi_save_config(sock, ssid, password) < 0;
      }

    pthread_mutex_lock(&g_wifi_lock);
    if (!cancelled && (g_wifi_stop_requested || !g_wifi_ready))
      {
        cancelled = true;
      }
    close(sock);
    if (cancelled)
      {
        g_wifi_status.state = VG_WIFI_FAILED;
        g_wifi_status.ip_ready = false;
        g_wifi_status.ip[0] = '\0';
        snprintf(g_wifi_status.error, sizeof(g_wifi_status.error),
                 "%s", "Wi-Fi 操作已取消");
      }
    else if (associated)
      {
        g_wifi_status.state = ip_ready ? VG_WIFI_CONNECTED
                                       : VG_WIFI_ASSOCIATED;
        g_wifi_status.ip_ready = ip_ready;
        g_wifi_status.error[0] = '\0';
        if (save_failed)
          {
            snprintf(g_wifi_status.error, sizeof(g_wifi_status.error),
                     "%s", "已连接，但保存配置失败");
          }
        else if (!ip_ready)
          {
            snprintf(g_wifi_status.error, sizeof(g_wifi_status.error), "%s",
                     dhcp_reset_failed ? "清除旧 IP 失败，未请求 DHCP" :
                     "Wi-Fi 已连接，尚未获取 IP");
          }
      }
    else
      {
        g_wifi_status.state = VG_WIFI_FAILED;
        snprintf(g_wifi_status.error, sizeof(g_wifi_status.error),
                 "%s", "SSID 或密码错误，未建立连接");
      }
    pthread_mutex_unlock(&g_wifi_lock);
  }
  memset(password, 0, sizeof(password));
  vg_wifi_worker_done();
  return NULL;
}

static void *vg_wifi_scan_entry(void *arg)
{
  void *ret = vg_wifi_scan_worker(arg);
  vg_wifi_worker_exit();
  return ret;
}

static void *vg_wifi_connect_entry(void *arg)
{
  void *ret = vg_wifi_connect_worker(arg);
  vg_wifi_worker_exit();
  return ret;
}
#endif

#ifdef CONFIG_WIRELESS_WAPI
static int vg_wifi_start_worker_locked(vg_wifi_worker_kind_t kind)
{
  pthread_attr_t attr;
  pthread_t thread;
  void *(*worker)(void *) = kind == VG_WIFI_WORKER_SCAN ?
                            vg_wifi_scan_entry : vg_wifi_connect_entry;
  bool attr_initialized = false;
  int ret;

  if (g_wifi_stopping || g_wifi_worker_running || !g_wifi_worker_exited)
    {
      return -EBUSY;
    }

  g_wifi_worker_running = true;
  g_wifi_worker_exited = false;
  g_wifi_status.operation_busy = true;
#ifndef __NuttX__
  g_wifi_worker_valid = false;
  g_wifi_worker_reaping = false;
#endif
  g_wifi_stop_requested = false;
  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      attr_initialized = true;
      ret = pthread_attr_setstacksize(&attr, CONFIG_VELAGUARD_WIFI_STACKSIZE);
    }
#ifdef __NuttX__
  /* Wi-Fi 可能由 LVGL 或 NSH 启动，不能用 task_create 的父任务组
   * 规则回收；分离式 pthread 由退出路径自行释放资源。
   */
  if (ret == 0)
    {
      ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }
#endif
  if (ret == 0)
    {
      ret = pthread_create(&thread, &attr, worker, NULL);
    }
  if (ret != 0)
    {
      g_wifi_worker_running = false;
      g_wifi_worker_exited = true;
      g_wifi_status.operation_busy = false;
      if (attr_initialized)
        {
          pthread_attr_destroy(&attr);
        }
      return -EAGAIN;
    }
  if (attr_initialized)
    {
      pthread_attr_destroy(&attr);
    }
#ifndef __NuttX__
  g_wifi_worker = thread;
  g_wifi_worker_valid = true;
#endif
  return 0;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vg_wifi_init(void)
{
  if (vg_wifi_reap_worker() < 0)
    {
      return -EAGAIN;
    }
  pthread_mutex_lock(&g_wifi_lock);
  if (g_wifi_stopping || !g_wifi_worker_exited || g_wifi_worker_running)
    {
      pthread_mutex_unlock(&g_wifi_lock);
      return -EBUSY;
    }

  memset(&g_wifi_status, 0, sizeof(g_wifi_status));
  memset(g_wifi_networks, 0, sizeof(g_wifi_networks));
  g_wifi_status.state = VG_WIFI_IDLE;
  g_wifi_worker_running = false;
  g_wifi_worker_exited = true;
  g_wifi_stop_requested = false;
  g_wifi_stopping = false;
  g_wifi_ready = true;
#ifdef CONFIG_WIRELESS_WAPI
  g_pending_ssid[0] = '\0';
  g_pending_password[0] = '\0';
#endif
  pthread_mutex_unlock(&g_wifi_lock);
  return 0;
}

void vg_wifi_deinit(void)
{
  struct timespec deadline;
  int wait_ret;

  pthread_mutex_lock(&g_wifi_lock);
  g_wifi_ready = false;
  g_wifi_stop_requested = true;
  g_wifi_stopping = !g_wifi_worker_exited;
#ifdef CONFIG_WIRELESS_WAPI
  memset(g_pending_password, 0, sizeof(g_pending_password));
#endif

  if (clock_gettime(CLOCK_REALTIME, &deadline) == 0)
    {
      deadline.tv_sec += VG_WIFI_DEINIT_TIMEOUT_MS / 1000;
      deadline.tv_nsec += (VG_WIFI_DEINIT_TIMEOUT_MS % 1000) * 1000000L;
      if (deadline.tv_nsec >= 1000000000L)
        {
          deadline.tv_sec++;
          deadline.tv_nsec -= 1000000000L;
        }
    }
  else
    {
      deadline.tv_sec = time(NULL) + VG_WIFI_DEINIT_TIMEOUT_MS / 1000;
      deadline.tv_nsec = 0;
    }

  while (g_wifi_worker_running || g_wifi_stopping)
    {
      wait_ret = pthread_cond_timedwait(&g_wifi_worker_cond, &g_wifi_lock,
                                        &deadline);
      if (wait_ret == ETIMEDOUT)
        {
          printf("[velaguard] Wi-Fi 任务关闭等待超时，保留任务状态供下次初始化回收\n");
          break;
        }
      if (wait_ret != 0)
        {
          printf("[velaguard] Wi-Fi 任务关闭等待失败: %d\n", wait_ret);
          break;
        }
    }
  pthread_mutex_unlock(&g_wifi_lock);
  (void)vg_wifi_reap_worker();
}

int vg_wifi_start_scan(void)
{
#ifdef CONFIG_WIRELESS_WAPI
  int ret;

  if (vg_wifi_reap_worker() < 0)
    {
      vg_wifi_set_error("上一次 Wi-Fi 任务回收失败");
      return -EAGAIN;
    }
  pthread_mutex_lock(&g_wifi_lock);
  if (g_wifi_stopping)
    {
      ret = -EBUSY;
    }
  else if (!g_wifi_ready)
    {
      ret = -ESHUTDOWN;
    }
  else if (g_wifi_worker_running || !g_wifi_worker_exited)
    {
      ret = -EBUSY;
    }
  else
    {
      g_wifi_status.state = VG_WIFI_SCANNING;
      g_wifi_status.error[0] = '\0';
      ret = vg_wifi_start_worker_locked(VG_WIFI_WORKER_SCAN);
    }
  pthread_mutex_unlock(&g_wifi_lock);
  if (ret < 0)
    {
      if (ret == -EAGAIN)
        {
          vg_wifi_set_error("扫描任务启动失败");
        }
      else if (ret == -ESHUTDOWN)
        {
          vg_wifi_set_error("Wi-Fi 模块未初始化");
        }
      else if (ret == -EBUSY)
        {
          vg_wifi_set_notice("上一次 Wi-Fi 操作仍在结束，请稍候");
        }
    }
  return ret;
#else
  vg_wifi_set_error("当前固件未启用 WAPI");
  return -ENOTSUP;
#endif
}

int vg_wifi_select(int delta)
{
  unsigned int count;

  pthread_mutex_lock(&g_wifi_lock);
  count = g_wifi_status.network_count;
  if (g_wifi_worker_running || !g_wifi_worker_exited)
    {
      snprintf(g_wifi_status.error, sizeof(g_wifi_status.error), "%s",
               "Wi-Fi 扫描尚未结束，请稍候");
      pthread_mutex_unlock(&g_wifi_lock);
      return -EBUSY;
    }

  if (count == 0)
    {
      snprintf(g_wifi_status.error, sizeof(g_wifi_status.error), "%s",
               "请先扫描 Wi-Fi 热点");
      pthread_mutex_unlock(&g_wifi_lock);
      return -ENOENT;
    }

  if (delta < 0)
    {
      g_wifi_status.selected = g_wifi_status.selected == 0 ? count - 1 :
                               g_wifi_status.selected - 1;
    }
  else if (delta > 0)
    {
      g_wifi_status.selected = (g_wifi_status.selected + 1) % count;
    }
  snprintf(g_wifi_status.ssid, sizeof(g_wifi_status.ssid), "%s",
           g_wifi_networks[g_wifi_status.selected].ssid);
  pthread_mutex_unlock(&g_wifi_lock);
  return 0;
}

int vg_wifi_get_network(unsigned int index, vg_wifi_network_t *network)
{
  if (network == NULL || index >= VG_WIFI_MAX_NETWORKS)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_wifi_lock);
  if (index >= g_wifi_status.network_count)
    {
      pthread_mutex_unlock(&g_wifi_lock);
      return -ENOENT;
    }
  *network = g_wifi_networks[index];
  pthread_mutex_unlock(&g_wifi_lock);
  return 0;
}

int vg_wifi_get_status(vg_wifi_status_t *status)
{
  if (status == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_wifi_lock);
  *status = g_wifi_status;
  pthread_mutex_unlock(&g_wifi_lock);
  return 0;
}

int vg_wifi_connect(const char *ssid, const char *password, bool save)
{
#ifdef CONFIG_WIRELESS_WAPI
  size_t ssid_len;
  size_t password_len;
  int ret;

  if (ssid == NULL || password == NULL)
    {
      vg_wifi_set_error("Wi-Fi 参数为空");
      return -EINVAL;
    }

  if (vg_wifi_reap_worker() < 0)
    {
      vg_wifi_set_error("上一次 Wi-Fi 任务回收失败");
      return -EAGAIN;
    }
  ssid_len = strlen(ssid);
  password_len = strlen(password);
  if (ssid_len == 0 || ssid_len >= VG_WIFI_SSID_LEN ||
      password_len >= VG_WIFI_PASSWORD_LEN ||
      (password_len > 0 && password_len < 8))
    {
      vg_wifi_set_error(password_len > 0 && password_len < 8 ?
                        "Wi-Fi 密码至少需要 8 位" :
                        "Wi-Fi 名称或密码长度不合法");
      return -EINVAL;
    }

  pthread_mutex_lock(&g_wifi_lock);
  if (g_wifi_stopping)
    {
      ret = -EBUSY;
    }
  else if (!g_wifi_ready)
    {
      ret = -ESHUTDOWN;
    }
  else if (g_wifi_worker_running || !g_wifi_worker_exited)
    {
      ret = -EBUSY;
    }
  else
    {
      snprintf(g_pending_ssid, sizeof(g_pending_ssid), "%s", ssid);
      snprintf(g_pending_password, sizeof(g_pending_password), "%s", password);
      g_pending_save = save;
      g_wifi_status.state = VG_WIFI_CONNECTING;
      g_wifi_status.ip_ready = false;
      g_wifi_status.ip[0] = '\0';
      snprintf(g_wifi_status.ssid, sizeof(g_wifi_status.ssid), "%s", ssid);
      g_wifi_status.error[0] = '\0';
      ret = vg_wifi_start_worker_locked(VG_WIFI_WORKER_CONNECT);
      if (ret < 0)
        {
          memset(g_pending_password, 0, sizeof(g_pending_password));
          g_wifi_status.state = VG_WIFI_FAILED;
          g_wifi_status.ip_ready = false;
          g_wifi_status.ip[0] = '\0';
        }
    }
  pthread_mutex_unlock(&g_wifi_lock);

  if (ret == -EAGAIN)
    {
      vg_wifi_set_error("连接任务启动失败");
    }
  else if (ret == -ESHUTDOWN)
    {
      vg_wifi_set_error("Wi-Fi 模块未初始化");
    }
  else if (ret == -EBUSY)
    {
      vg_wifi_set_notice("上一次 Wi-Fi 操作仍在结束，请稍候");
    }
  return ret;
#else
  (void)ssid;
  (void)password;
  (void)save;
  vg_wifi_set_error("当前固件未启用 WAPI");
  return -ENOTSUP;
#endif
}

int vg_wifi_connect_selected(const char *password, bool save)
{
  vg_wifi_status_t status;

  if (password == NULL)
    {
      password = "";
    }

  if (vg_wifi_get_status(&status) < 0 || status.network_count == 0 ||
      status.selected >= status.network_count || status.ssid[0] == '\0')
    {
      vg_wifi_set_error("请先扫描并选择 Wi-Fi");
      return -ENOENT;
    }

  return vg_wifi_connect(status.ssid, password, save);
}

const char *vg_wifi_state_text(vg_wifi_state_t state, bool ascii)
{
  switch (state)
    {
      case VG_WIFI_SCANNING:
        return ascii ? "SCANNING" : "扫描中";
      case VG_WIFI_SCAN_READY:
        return ascii ? "SCAN READY" : "扫描完成";
      case VG_WIFI_CONNECTING:
        return ascii ? "CONNECTING" : "连接中";
      case VG_WIFI_ASSOCIATED:
        return ascii ? "ASSOCIATED, NO IP" : "已关联，未分配 IP";
      case VG_WIFI_CONNECTED:
        return ascii ? "CONNECTED" : "已连接";
      case VG_WIFI_FAILED:
        return ascii ? "FAILED" : "失败";
      default:
        return ascii ? "IDLE" : "未配置";
    }
}
