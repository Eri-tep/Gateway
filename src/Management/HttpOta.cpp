#include "MgmtRpc.h"
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

HttpOtaState g_http_ota_state{};


static void Task_HttpOta(void *pvParameters) {
  char *url = static_cast<char *>(pvParameters);
  if (!url) {
    g_http_ota_state.in_progress = false;
    vTaskDelete(nullptr);
    return;
  }

  ::Serial.printf("[OTA] Starting HTTP(S) Stream OTA from URL: %s\r\n", url);
  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Connecting...");
  g_http_ota_state.progress_pct = 0;
  g_http_ota_state.last_error[0] = '\0';

  // OTA 시작 즉시 시스템 상태를 OTA 모드로 전환하여 네트워크/워치독 충돌 방지
  g_ota_in_progress.store(true, std::memory_order_release);
  if (g_system_event_group) {
    xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }

  bool is_https = (strncmp(url, "https://", 8) == 0);
  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  WiFiClient *client_ptr = nullptr;

  if (is_https) {
    secure_client.setInsecure(); // GitHub CDN(objects.githubusercontent.com) 리다이렉트 HTTPS 허용
    secure_client.setHandshakeTimeout(15);
    client_ptr = &secure_client;
  } else {
    client_ptr = &plain_client;
  }

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  if (!http.begin(*client_ptr, url)) {
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error), "HTTP begin failed");
    ::Serial.println(F("[OTA] HTTP begin connection failed."));
    g_http_ota_state.in_progress = false;
    g_ota_in_progress.store(false, std::memory_order_release);
    if (g_system_event_group) xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    free(url);
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
    free(url);
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
    free(url);
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
    free(url);
    vTaskDelete(nullptr);
    return;
  }

  esp_task_wdt_add(nullptr);
  WiFiClient *stream = http.getStreamPtr();
  static uint8_t s_ota_buff[4096]; // Flash 4KB Sector 일치 정적 버퍼 (Zero Heap / Zero Stack)
  size_t written = 0;
  uint32_t last_progress_ms = 0;
  uint32_t last_activity_ms = millis();

  while (http.connected() && (written < static_cast<size_t>(contentLength))) {
    esp_task_wdt_reset();
    int c = stream->read(s_ota_buff, sizeof(s_ota_buff));
    if (c > 0) {
      last_activity_ms = millis();
      size_t w = Update.write(s_ota_buff, c);
      if (w != static_cast<size_t>(c)) {
        snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
        snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error), "Flash write error at %u", (unsigned)written);
        ::Serial.printf("[OTA] %s\r\n", g_http_ota_state.last_error);
        break;
      }
      written += w;
      uint32_t now = millis();
      if (now - last_progress_ms >= 200 || written == static_cast<size_t>(contentLength)) {
        last_progress_ms = now;
        uint8_t pct = static_cast<uint8_t>((written * 100) / contentLength);
        g_http_ota_state.progress_pct = pct;
        snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Downloading (%u%%)", pct);
      }
    } else {
      if (millis() - last_activity_ms > 20000) {
        snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Timeout");
        snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error), "Stream read timeout (20s)");
        ::Serial.println(F("[OTA] Stream read timeout."));
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
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
  free(url);
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

  char *url_copy = strdup(target_url);
  if (!url_copy) {
    g_http_ota_state.in_progress = false;
    return;
  }

  // Core 1에서 최고 우선순위(15)로 실행: Core 0의 Wi-Fi/LwIP 네트워크 수신 스레드와 코어를 분리하여 병렬 최대 throughput 보장
  xTaskCreatePinnedToCore(Task_HttpOta, "HttpOtaTask", 10240, url_copy, 15, nullptr, 1);
}

