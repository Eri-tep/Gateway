#include "NetworkInternal.h"
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <ArduinoOTA.h>
#include <algorithm>

namespace {
constexpr uint32_t POST_BOOT_LOG_DELAY_MS = 5000;
} // namespace

EventGroupHandle_t g_wifi_event_group = nullptr;
static uint32_t s_wifi_disconnect_count = 0;
static uint32_t s_last_sta_retry_ms = 0;

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println(F("[WIFI EVENT] STA Started"));
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println(F("[WIFI EVENT] STA Connected to AP"));
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_CONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    s_wifi_disconnect_count = 0;
    Serial.printf("[WIFI EVENT] STA Got IP: %s\r\n",
                  IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_GOT_IP);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    s_wifi_disconnect_count++;
    Serial.printf("[WIFI EVENT] STA Disconnected (Reason: %d, Count: %u)\r\n",
                  info.wifi_sta_disconnected.reason, s_wifi_disconnect_count);
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_CONNECTED);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_START:
    Serial.println(F("[WIFI EVENT] SoftAP Started"));
    break;
  case ARDUINO_EVENT_WIFI_AP_STOP:
    Serial.println(F("[WIFI EVENT] SoftAP Stopped"));
    break;
  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    Serial.printf(
        "[WIFI EVENT] AP Station Connected! MAC: "
        "%02X:%02X:%02X:%02X:%02X:%02X, AID: %d\r\n",
        info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
        info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
        info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5],
        info.wifi_ap_staconnected.aid);
    break;
  case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
    Serial.printf("[WIFI EVENT] AP Station Disconnected! MAC: "
                  "%02X:%02X:%02X:%02X:%02X:%02X, AID: %d\r\n",
                  info.wifi_ap_stadisconnected.mac[0],
                  info.wifi_ap_stadisconnected.mac[1],
                  info.wifi_ap_stadisconnected.mac[2],
                  info.wifi_ap_stadisconnected.mac[3],
                  info.wifi_ap_stadisconnected.mac[4],
                  info.wifi_ap_stadisconnected.mac[5],
                  info.wifi_ap_stadisconnected.aid);
    break;
  default:
    break;
  }
}


void Task_Network(void *pvParameters) {
  esp_task_wdt_add(nullptr);
  uint32_t t_chk = millis(), t_met = millis(), t_tcp = millis();
  uint32_t t_dp_heartbeat = millis();
  uint32_t last_ch5_activity_ms = 0;

  int door_server_fd = -1;
  int mgmt_server_fd = -1;
  int ew11_server_fds[Config::TCP::MAX_EW11_SLOTS];
  for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
    ew11_server_fds[s] = -1;
  }

  if (!g_rescue_mode.load(std::memory_order_relaxed)) {
    mgmt_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mgmt_server_fd >= 0) {
      int opt = 1;
      setsockopt(mgmt_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      int flags = fcntl(mgmt_server_fd, F_GETFL, 0);
      fcntl(mgmt_server_fd, F_SETFL, flags | O_NONBLOCK);

      struct sockaddr_in saddr;
      memset(&saddr, 0, sizeof(saddr));
      saddr.sin_family = AF_INET;
      saddr.sin_addr.s_addr = htonl(INADDR_ANY);
      saddr.sin_port = htons(Config::TCP::MGMT_PORT);
      if (bind(mgmt_server_fd, reinterpret_cast<struct sockaddr *>(&saddr),
               sizeof(saddr)) < 0 ||
          listen(mgmt_server_fd, Config::TCP::MAX_MGMT_CLIENTS) < 0) {
        ESP_LOGE("NET", "Failed to bind/listen mgmt server (8900): errno %d",
                 errno);
        close(mgmt_server_fd);
        mgmt_server_fd = -1;
      }
    }

    Hub_LoadConfig();

    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      uint16_t listen_port = g_hub_slots[s].target_port;
      if (listen_port == 0) {
        listen_port = Config::TCP::EW11_SLOT_PORTS[s];
        g_hub_slots[s].target_port = listen_port;
      }

      int sfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (sfd >= 0) {
        int opt = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int flags = fcntl(sfd, F_GETFL, 0);
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_ANY);
        saddr.sin_port = htons(listen_port);
        if (bind(sfd, reinterpret_cast<struct sockaddr *>(&saddr),
                 sizeof(saddr)) < 0 ||
            listen(sfd, 1) < 0) {
          ESP_LOGE("EW11", "Failed to bind/listen EW11 slot %d on port %u: errno %d",
                   s, listen_port, errno);
          close(sfd);
          sfd = -1;
        } else {
          ESP_LOGI("EW11", "[CH5] Listening for EW11 slot %d (%s) on port %u",
                   s, g_hub_slots[s].name, listen_port);
        }
      }
      ew11_server_fds[s] = sfd;
    }
  } else {
    Serial.println(F("[RESCUE] CH6 TCP server port disabled in Rescue "
                     "Mode. Dedicated to OTA & Telnet."));
  }

  for (;;) {
    esp_task_wdt_reset();
    g_wdt_monitor.feed(4);
    ArduinoOTA.handle();
    if (g_ota_in_progress.load(std::memory_order_relaxed)) {
      esp_task_wdt_reset();
      g_wdt_monitor.feed(4);
      ArduinoOTA.handle();
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    System_CheckOtaHealth();
    Cache_CheckNvsDebounce();

    if (!g_rescue_mode.load(std::memory_order_relaxed) && g_wifi_event_group) {
      EventBits_t bits = xEventGroupGetBits(g_wifi_event_group);

      if (bits & WIFI_BIT_GOT_IP) {
        xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_GOT_IP);
        if (g_wifi_guard.testing.load(std::memory_order_acquire)) {
          g_wifi_guard.testing.store(false, std::memory_order_release);
          Config_Save();
          Serial.printf("[WIFI] ★ New Wi-Fi '%s' connected successfully! Saved "
                        "to NVS.\r\n",
                        g_config.wifi_ssid);
        }
        if (WiFi.getMode() == WIFI_MODE_APSTA ||
            WiFi.getMode() == WIFI_MODE_AP) {
          WiFi.softAPdisconnect(true);
          WiFi.mode(WIFI_STA);
          WiFi.setSleep(false);
          Serial.println(F("[WIFI] Event: GOT_IP! Fallback SoftAP disabled, "
                           "restored STA mode."));
        }
        configTime(0, 0, "pool.ntp.org", "asia.pool.ntp.org");
        setenv("TZ", "KST-9", 1);
        tzset();
      }

      if (g_wifi_guard.testing.load(std::memory_order_acquire)) {
        if (TimeUtils::isElapsed(g_wifi_guard.start_ms, 15000)) {
          g_wifi_guard.testing.store(false, std::memory_order_release);
          Serial.printf("[WIFI] ⚠️ New Wi-Fi '%s' failed to connect within 15s! "
                        "Reverting to '%s'...\r\n",
                        g_config.wifi_ssid, g_wifi_guard.prev_ssid);
          strncpy(g_config.wifi_ssid, g_wifi_guard.prev_ssid,
                  sizeof(g_config.wifi_ssid) - 1);
          strncpy(g_config.wifi_password, g_wifi_guard.prev_pass,
                  sizeof(g_config.wifi_password) - 1);
          WiFi.disconnect(false);
          vTaskDelay(pdMS_TO_TICKS(100));
          WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
        }
      }

      if (bits & WIFI_BIT_DISCONNECTED) {
        if (TimeUtils::isElapsed(
                s_last_sta_retry_ms,
                Config::Timing::WIFI_BACKGROUND_RETRY_INTERVAL_MS)) {
          s_last_sta_retry_ms = millis();
          Serial.println(F("[WIFI] Event: DISCONNECTED. Background STA "
                           "reconnection attempt..."));
          esp_wifi_connect();
        }
      }
    }

    if (s_pending_reboot_reason &&
        millis() > POST_BOOT_LOG_DELAY_MS) {
      LogManager::writeRebootLog(s_pending_reboot_reason);
      s_pending_reboot_reason = nullptr;
    }

    fd_set readfds, writefds, errorfds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&errorfds);
    int max_fd = -1;

    auto add_read_fd = [&](int fd) {
      if (fd >= 0) {
        FD_SET(fd, &readfds);
        FD_SET(fd, &errorfds);
        if (fd > max_fd)
          max_fd = fd;
      }
    };

    add_read_fd(mgmt_server_fd);
    for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
      if (ew11_server_fds[s] >= 0) {
        add_read_fd(ew11_server_fds[s]);
      }
    }

    {
      MutexLocker lock(g_mgmt_mutex);
      for (int m = 0; m < Config::TCP::MAX_MGMT_CLIENTS; m++) {
        if (g_mgmt_sessions[m].sock >= 0) {
          add_read_fd(g_mgmt_sessions[m].sock);
        }
      }
    }

    {
      MutexLocker lock(g_ch5_mutex);
      for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
        if (g_hub_slots[s].sock >= 0) {
          add_read_fd(g_hub_slots[s].sock);
        }
      }
    }

    struct timeval tv = {0, 10000}; // 10ms 커널 레벨 Event-Driven 블로킹 (소켓
    int act = select(max_fd + 1, &readfds, &writefds, &errorfds, &tv);

    if (act > 0) {
      if (mgmt_server_fd >= 0 && FD_ISSET(mgmt_server_fd, &readfds)) {
        Tcp_AcceptAndAssignSlot(mgmt_server_fd, g_mgmt_sessions, g_mgmt_mutex,
                                Config::TCP::DEFAULT_KEEPALIVE_IDLE_SEC,
                                Config::TCP::DEFAULT_KEEPALIVE_INTVL_SEC,
                                Config::TCP::DEFAULT_KEEPALIVE_CNT,
                                g_pkt_stats.ch6);
      }

      for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
        if (ew11_server_fds[s] >= 0 && FD_ISSET(ew11_server_fds[s], &readfds)) {
          Hub_AcceptClient(s, ew11_server_fds[s]);
        }
      }

      Tcp_PollAndReceive(g_mgmt_sessions, g_mgmt_mutex, readfds, errorfds,
                         [](MgmtSession *s, const uint8_t *data, size_t len) {
                           Mgmt_Data(s, data, len);
                         });

      {
        MutexLocker lock(g_ch5_mutex);
        for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
          auto &slot = g_hub_slots[s];
          if (slot.sock < 0)
            continue;

          if (FD_ISSET(slot.sock, &errorfds)) {
            close(slot.sock);
            slot.sock = -1;
            slot.is_connected = false;
            slot.rx_len = 0;
            ESP_LOGW("EW11", "[CH5] Slot %d (%s) socket error detected. Closed.", s, slot.name);
            continue;
          }

          if (FD_ISSET(slot.sock, &readfds)) {
            uint8_t temp_buf[Config::TCP::POLL_RX_CHUNK_SIZE];
            int r = recv(slot.sock, temp_buf, sizeof(temp_buf), 0);
            if (r > 0) {
              Hub_Data(&slot, temp_buf, r);
            } else if (r == 0 ||
                       (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
              close(slot.sock);
              slot.sock = -1;
              slot.is_connected = false;
              slot.rx_len = 0;
              ESP_LOGI("EW11", "[CH5] Slot %d (%s) disconnected by peer.", s, slot.name);
            }
          }
        }
      }
    }


    uint32_t now = millis();
    if (TimeUtils::isElapsed(t_chk,
                             Config::Timing::SYSTEM_MONITOR_INTERVAL_MS)) {
      t_chk = now;
      size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      if (!g_http_ota_state.in_progress.load(std::memory_order_relaxed)) {
        if (free_heap < Config::Memory::MIN_HEAP_THRESHOLD_KB * 1024) {
          System_Restart("Low Heap Memory");
        }
      } else {
        if (free_heap < 8 * 1024) {
          System_Restart("Low Heap Memory (OTA)");
        }
      }
    }

    if (TimeUtils::isElapsed(t_met, Config::Metrics::SAMPLE_INTERVAL_MS)) {
      t_met = now;
      uint16_t used_ram = (heap_caps_get_total_size(MALLOC_CAP_8BIT) -
                           heap_caps_get_free_size(MALLOC_CAP_8BIT)) /
                          1024;
      uint8_t c0 = 0, c1 = 0;
      System_ReadCpuPct(c0, c1);
      g_metrics.addSample(c0, c1, used_ram, System_ReadTempC());
    }

    if (TimeUtils::isElapsed(t_tcp, Config::TCP::CLEANUP_INTERVAL_MS)) {
      t_tcp = now;

      bool any_ew11_conn = false;
      {
        MutexLocker lock(g_ch5_mutex);
        for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
          if (g_hub_slots[s].is_connected) {
            any_ew11_conn = true;
            break;
          }
        }
      }
      g_pkt_stats.ch5.is_connected.store(any_ew11_conn,
                                         std::memory_order_relaxed);

      g_pkt_stats.ch6.is_connected.store(
          Tcp_HasActiveSession(g_mgmt_sessions, g_mgmt_mutex),
          std::memory_order_relaxed);
    }
  }
}
