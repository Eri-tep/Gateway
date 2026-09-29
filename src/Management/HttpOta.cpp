#include "MgmtRpc.h"
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

HttpOtaState g_http_ota_state{};


static char s_ota_target_url[256] = {0};

static bool is_trusted_ota_url(const char *url) {
  if (!url || strncmp(url, "https://", 8) != 0)
    return false;

  const char *host_start = url + 8;
  const char *slash = strchr(host_start, '/');
  size_t host_len = slash ? static_cast<size_t>(slash - host_start) : strlen(host_start);
  char host[128] = {0};
  if (host_len == 0 || host_len >= sizeof(host))
    return false;
  memcpy(host, host_start, host_len);
  host[host_len] = '\0';

  static const char *const TRUSTED_DOMAINS[] = {
      "raw.githubusercontent.com",
      "objects.githubusercontent.com",
      "github.com",
      "github-releases.githubusercontent.com"
  };
  for (const auto *domain : TRUSTED_DOMAINS) {
    if (strcasecmp(host, domain) == 0)
      return true;
  }
  return false;
}

static void Task_HttpOta(void *pvParameters) {
  const char *url = static_cast<const char *>(pvParameters);
  if (!url || strlen(url) == 0) {
    g_http_ota_state.in_progress = false;
    vTaskDelete(nullptr);
    return;
  }

  ::Serial.printf("[OTA] Starting Stream OTA from URL: %s\r\n", url);
  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Connecting...");
  g_http_ota_state.progress_pct = 0;
  g_http_ota_state.last_error[0] = '\0';

  g_ota_in_progress.store(true, std::memory_order_release);
  if (g_system_event_group) {
    xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }

  bool is_https = (strncmp(url, "https://", 8) == 0);
  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  WiFiClient *client_ptr = nullptr;

  if (is_https) {
    secure_client.setInsecure();
    secure_client.setHandshakeTimeout(5);
    secure_client.setTimeout(5);
    client_ptr = &secure_client;
  } else {
    plain_client.setTimeout(5);
    client_ptr = &plain_client;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(5000);

  if (!http.begin(*client_ptr, url)) {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error), "HTTP begin failed");
    ::Serial.println(F("[OTA] HTTP begin connection failed."));
    g_http_ota_state.in_progress = false;
    g_ota_in_progress.store(false, std::memory_order_release);
    if (g_system_event_group) xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    vTaskDelete(nullptr);
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "HTTP error (%d): %s", httpCode, http.errorToString(httpCode).c_str());
    ::Serial.printf("[OTA] HTTP GET error: %s\r\n", g_http_ota_state.last_error);
    http.end();
    g_http_ota_state.in_progress = false;
    g_ota_in_progress.store(false, std::memory_order_release);
    if (g_system_event_group) xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    vTaskDelete(nullptr);
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error), "Invalid Content-Length: %d", contentLength);
    ::Serial.printf("[OTA] Invalid firmware size: %d\r\n", contentLength);
    http.end();
    g_http_ota_state.in_progress = false;
    g_ota_in_progress.store(false, std::memory_order_release);
    if (g_system_event_group) xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    vTaskDelete(nullptr);
    return;
  }

  ::Serial.printf("[OTA] Firmware binary size: %d bytes. Preparing flash partition...\r\n", contentLength);

  if (!Update.begin(contentLength, U_FLASH)) {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Update.begin failed (0x%x)", Update.getError());
    ::Serial.printf("[OTA] Update.begin failed: %s\r\n", g_http_ota_state.last_error);
    http.end();
    g_http_ota_state.in_progress = false;
    g_ota_in_progress.store(false, std::memory_order_release);
    if (g_system_event_group) xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    vTaskDelete(nullptr);
    return;
  }

  esp_task_wdt_add(nullptr);

  static uint8_t s_ota_buff[4096]; // Flash 4KB Sector 일치 정적 버퍼 (Zero Heap / Zero Stack)
  WiFiClient *stream = http.getStreamPtr();
  size_t written = 0;
  uint32_t last_progress_time = millis();

  while (written < static_cast<size_t>(contentLength)) {
    esp_task_wdt_reset();

    size_t avail = stream->available();
    if (avail > 0) {
      size_t read_bytes = stream->readBytes(s_ota_buff, std::min(avail, sizeof(s_ota_buff)));
      if (read_bytes > 0) {
        Update.write(s_ota_buff, read_bytes);
        taskYIELD(); // Flash 쓰기 직후 Wi-Fi 스택이 TCP ACK 송출할 수 있도록 협조적 양보
        written += read_bytes;
        last_progress_time = millis(); // 데이터 수신 시 스톨 타이머 리셋

        int pct = (written * 100) / contentLength;
        g_http_ota_state.progress_pct = pct;

        if (millis() - last_progress_time >= 1000) {
          last_progress_time = millis();
          snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Downloading (%d%%)", pct);
          ::Serial.printf("[OTA] Progress: %d%% (%u / %d bytes)\r\n", pct, (unsigned)written, contentLength);
        }
      }
    } else {
      // 데이터 없음: 협조적 양보 후 스톨 감지
      taskYIELD();
      if (millis() - last_progress_time > 3000) {
        ::Serial.println(F("[OTA] Read stall timeout (3s), aborting."));
        break; // 3초간 데이터 없음 → 네트워크 단절로 처리
      }
    }
  }

  esp_task_wdt_delete(nullptr);

  if (written == static_cast<size_t>(contentLength) && Update.end(true)) {
    if (Update.isFinished()) {
      snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Success (Rebooting)");
      ::Serial.println(F("[OTA] Update OK! Rebooting into new firmware in 1s..."));
      http.end();
      vTaskDelay(pdMS_TO_TICKS(1000));
      System_Restart("HTTP OTA Update");
    }
  } else {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Update write failed (written %u / %d, err: 0x%x)",
             (unsigned)written, contentLength, Update.getError());
    ::Serial.printf("[OTA] Update failed: %s\r\n", g_http_ota_state.last_error);
  }

  http.end();
  g_http_ota_state.in_progress = false;
  g_ota_in_progress.store(false, std::memory_order_release);
  if (g_system_event_group) {
    xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }
  vTaskDelete(nullptr);
}

void Mgmt_StartHttpOta(const char *url) {
  const char *target_url = (url && strlen(url) > 0) ? url : DEFAULT_CLOUD_OTA_URL;
  if (g_http_ota_state.in_progress.load()) {
    ::Serial.println(F("[OTA] Update already in progress, ignoring duplicate request."));
    return;
  }

  g_http_ota_state.in_progress = true;
  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Starting...");
  g_http_ota_state.progress_pct = 0;
  g_http_ota_state.last_error[0] = '\0';

  strncpy(s_ota_target_url, target_url, sizeof(s_ota_target_url) - 1);
  s_ota_target_url[sizeof(s_ota_target_url) - 1] = '\0';

  BaseType_t res = xTaskCreatePinnedToCore(Task_HttpOta, "HttpOtaTask", 10240, s_ota_target_url, 10, nullptr, 1);
  if (res != pdPASS) {
    g_http_ota_state.in_progress = false;
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error), "Task create failed");
    ::Serial.println(F("[OTA] Failed to create HttpOtaTask."));
  }
}
