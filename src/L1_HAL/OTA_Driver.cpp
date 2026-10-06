#include "L1_HAL/OTA_Driver.h"
#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

HttpOtaState g_http_ota_state{};
static char s_ota_target_url[256] = {0};

static constexpr char OTA_REPO_PREFIX[] = "/Eri-tep/Gateway/";
static constexpr size_t MAX_REDIRECT_LOCATION_LEN = 1024;

static PreOtaHookFn s_pre_ota_hook = nullptr;
void SystemOta_RegisterPreOtaHook(PreOtaHookFn hook) noexcept {
  s_pre_ota_hook = hook;
}

enum class OtaUrlContext { Initial, Redirect };

static void configure_public_tls(WiFiClientSecure &client) {
  // 이전 홉의 setInsecure() 잔여 상태(_use_insecure = true)를 확실히 해제
  client.setCACert(nullptr);

#if defined(ESP_ARDUINO_VERSION) &&                                            \
    (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 12))
  client.useBuiltinCACertBundle();
#else
  extern const uint8_t x509_crt_bundle_start[] asm(
      "_binary_x509_crt_bundle_start");
  extern const uint8_t x509_crt_bundle_end[] asm(
      "_binary_x509_crt_bundle_end");
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  client.setCACertBundle(x509_crt_bundle_start,
                         static_cast<size_t>(x509_crt_bundle_end - x509_crt_bundle_start));
#else
  client.setCACertBundle(x509_crt_bundle_start);
#endif
#endif
  client.setTimeout(5);
  client.setHandshakeTimeout(8); // CA 체인 검증을 감안하여 8초 확보
}

static bool is_private_host(const char *host) {
  if (!host)
    return false;
  if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0) {
    return true;
  }
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = '\0';
  if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 &&
      b < 256 && c < 256 && d < 256) {
    return a == 10 || (a == 192 && b == 168) ||
           (a == 172 && b >= 16 && b <= 31);
  }
  size_t n = strlen(host);
  return (n > 6 && strcasecmp(host + n - 6, ".local") == 0);
}

static bool has_unsafe_path_segments(const char *path) {
  if (!path)
    return true;
  return (strstr(path, "/./") != nullptr || strstr(path, "/../") != nullptr ||
          strstr(path, "/..") != nullptr || strstr(path, "/%2e") != nullptr ||
          strstr(path, "/%2E") != nullptr || strstr(path, "\\") != nullptr);
}

static bool extract_url_components(const char *url, char *out_host,
                                   size_t max_host_len, int &out_port,
                                   char *out_path, size_t max_path_len,
                                   bool &out_is_https) {
  if (!url)
    return false;
  out_is_https = (strncmp(url, "https://", 8) == 0);
  bool is_http = (strncmp(url, "http://", 7) == 0);
  if (!out_is_https && !is_http)
    return false;

  out_port = out_is_https ? 443 : 80;
  const char *host_start = out_is_https ? (url + 8) : (url + 7);

  // Authority 끝 지점 파싱: RFC 3986에 따라 '/', '?', '#' 중 가장 빠른 지점
  // 탐색
  const char *p = host_start;
  while (*p && *p != '/' && *p != '?' && *p != '#') {
    p++;
  }
  const char *auth_end = p;

  // 1. Userinfo ('@') 우회 공격 차단
  if (memchr(host_start, '@', static_cast<size_t>(auth_end - host_start)) !=
      nullptr) {
    return false;
  }

  // 2. 포트 번호 분리 및 엄격 검증
  const char *colon = static_cast<const char *>(
      memchr(host_start, ':', static_cast<size_t>(auth_end - host_start)));
  const char *host_end = colon ? colon : auth_end;
  size_t host_len = static_cast<size_t>(host_end - host_start);

  if (host_len == 0 || host_len >= max_host_len) {
    return false;
  }
  memcpy(out_host, host_start, host_len);
  out_host[host_len] = '\0';

  if (colon) {
    int port = 0;
    bool has_port_digits = false;
    for (const char *cp = colon + 1; cp < auth_end; ++cp) {
      if (*cp < '0' || *cp > '9')
        return false;
      has_port_digits = true;
      port = port * 10 + (*cp - '0');
      if (port > 65535)
        return false;
    }
    if (!has_port_digits || port == 0)
      return false;
    out_port = port;
  }

  // 3. Path 추출
  if (*auth_end == '/') {
    const char *path_end = auth_end;
    while (*path_end && *path_end != '?' && *path_end != '#') {
      path_end++;
    }
    size_t path_len = static_cast<size_t>(path_end - auth_end);
    if (path_len >= max_path_len)
      return false;
    memcpy(out_path, auth_end, path_len);
    out_path[path_len] = '\0';
  } else {
    if (max_path_len < 2)
      return false;
    out_path[0] = '/';
    out_path[1] = '\0';
  }

  return true;
}

static bool is_trusted_ota_url(const char *url, OtaUrlContext context) {
  char host[128] = {0};
  char path[256] = {0};
  int port = 0;
  bool is_https = false;
  if (!extract_url_components(url, host, sizeof(host), port, path, sizeof(path),
                              is_https)) {
    return false;
  }

  // 1. 로컬/사설망 IP 및 로컬 호스트 허용 (임의 포트, 임의 경로 허용)
  if (is_private_host(host)) {
    return true;
  }

  // 2. 외부 인터넷 도메인은 반드시 HTTPS + 443 포트여야 함
  if (!is_https || port != 443) {
    return false;
  }

  // 3. 공식 GitHub 저장소 호스트: 경로 검증 및 상위 디렉터리 순회(/../) 차단
  if (strcasecmp(host, "raw.githubusercontent.com") == 0 ||
      strcasecmp(host, "github.com") == 0) {
    if (has_unsafe_path_segments(path)) {
      return false;
    }
    return (strncmp(path, OTA_REPO_PREFIX, sizeof(OTA_REPO_PREFIX) - 1) == 0);
  }

  // 4. 공식 GitHub CDN 도메인: Redirect 컨텍스트에서만 허용 (최초 URL 직접 지정
  // 금지)
  if (strcasecmp(host, "objects.githubusercontent.com") == 0 ||
      strcasecmp(host, "release-assets.githubusercontent.com") == 0 ||
      strcasecmp(host, "github-releases.githubusercontent.com") == 0) {
    return (context == OtaUrlContext::Redirect);
  }

  return false;
}

class OtaInProgressGuard {
public:
  OtaInProgressGuard() {
    g_ota_in_progress.store(true, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
  }
  ~OtaInProgressGuard() {
    if (!_dismissed) {
      g_ota_in_progress.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
    }
  }
  void dismiss() noexcept { _dismissed = true; }
  OtaInProgressGuard(const OtaInProgressGuard &) = delete;
  OtaInProgressGuard &operator=(const OtaInProgressGuard &) = delete;

private:
  bool _dismissed{false};
};

static void ota_fail(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
            fmt, args);
  va_end(args);
  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Failed");
  ::Serial.printf("[OTA] Error: %s\r\n", g_http_ota_state.last_error);
}

static bool Ota_ResolveDownloadUrl(const char *initial_url,
                                   String &out_final_url,
                                   WiFiClientSecure &secure_client,
                                   WiFiClient &plain_client, HTTPClient &http,
                                   int &out_content_length) {
  char initial_host[128] = {0};
  char initial_path[256] = {0};
  int initial_port = 0;
  bool is_https_initial = false;
  extract_url_components(initial_url, initial_host, sizeof(initial_host),
                         initial_port, initial_path, sizeof(initial_path),
                         is_https_initial);

  if (!is_private_host(initial_host) && time(nullptr) < 1700000000) {
    ota_fail("System time not synced (NTP required for TLS)");
    return false;
  }

  ::Serial.printf("[OTA] Starting Stream OTA to target host: %s (port %d)\r\n",
                  initial_host, initial_port);
  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
           "Connecting...");
  g_http_ota_state.progress_pct = 0;
  g_http_ota_state.last_error[0] = '\0';

  // 사전 등록된 정리 훅 실행 (소켓 일시 해제 및 lwIP pcb + 수신 버퍼 힙 확보)
  if (s_pre_ota_hook) {
    s_pre_ota_hook();
  }

  esp_task_wdt_reset();

  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  const char *header_keys[] = {"Location"};
  http.collectHeaders(header_keys, 1);

  String current_url = initial_url;
  int redirect_count = 0;
  int httpCode = 0;

  ::Serial.printf("[OTA] Phase 1: Sending GET request...\r\n");

  while (redirect_count <= 2) {
    esp_task_wdt_reset();

    char host[128] = {0};
    char path[256] = {0};
    int port = 0;
    bool is_https = false;
    extract_url_components(current_url.c_str(), host, sizeof(host), port, path,
                           sizeof(path), is_https);

    OtaUrlContext ctx = (redirect_count == 0) ? OtaUrlContext::Initial
                                              : OtaUrlContext::Redirect;
    if (!is_trusted_ota_url(current_url.c_str(), ctx)) {
      ota_fail("Untrusted URL in chain (host: %s, port: %d)",
               host[0] ? host : "invalid", port);
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    WiFiClient *transport = nullptr;
    if (is_https) {
      if (is_private_host(host)) {
        secure_client.setInsecure();
      } else {
        configure_public_tls(secure_client);
      }
      transport = &secure_client;
    } else {
      plain_client.setTimeout(5);
      transport = &plain_client;
    }

    if (!http.begin(*transport, current_url)) {
      ota_fail("HTTP begin failed");
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      break;
    } else if (httpCode == HTTP_CODE_MOVED_PERMANENTLY ||
               httpCode == HTTP_CODE_FOUND || httpCode == HTTP_CODE_SEE_OTHER ||
               httpCode == HTTP_CODE_TEMPORARY_REDIRECT || httpCode == 308) {
      redirect_count++;
      if (redirect_count > 2) {
        ota_fail("Too many redirects (>2)");
        http.end();
        secure_client.stop();
        plain_client.stop();
        return false;
      }

      String location = http.header("Location");
      http.end();
      secure_client.stop();
      plain_client.stop();

      if (location.isEmpty() || location.length() > MAX_REDIRECT_LOCATION_LEN) {
        ota_fail("Invalid redirect Location length: %u",
                 (unsigned)location.length());
        return false;
      }

      if (!location.startsWith("http://") && !location.startsWith("https://")) {
        ota_fail("Relative redirect rejected");
        return false;
      }

      char next_host[128] = {0};
      char next_path[256] = {0};
      int next_port = 0;
      bool next_is_https = false;
      if (!extract_url_components(location.c_str(), next_host,
                                  sizeof(next_host), next_port, next_path,
                                  sizeof(next_path), next_is_https)) {
        ota_fail("Failed to parse redirect URL");
        return false;
      }

      if (is_private_host(next_host) != is_private_host(initial_host)) {
        ota_fail("Redirect crosses trust boundary");
        return false;
      }

      current_url = location;
      ::Serial.printf("[OTA] Redirect #%d (%d) -> to host: %s (port %d)\r\n",
                      redirect_count, httpCode, next_host, next_port);
    } else {
      ota_fail("HTTP GET failed, code: %d", httpCode);
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }
  }

  if (httpCode != HTTP_CODE_OK) {
    ota_fail("Failed to reach OK status (final code: %d)", httpCode);
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  out_content_length = http.getSize();
  ::Serial.printf("[OTA] Phase 2: HTTP 200 OK, image size: %d bytes\r\n",
                  out_content_length);
  if (out_content_length <= 0) {
    ota_fail("Invalid Content-Length: %d", out_content_length);
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  out_final_url = current_url;
  return true;
}

static bool Ota_StreamAndWritePartition(HTTPClient &http,
                                        WiFiClientSecure &secure_client,
                                        WiFiClient &plain_client,
                                        int contentLength) {
  esp_task_wdt_reset();

  if (!Update.begin(contentLength, U_FLASH)) {
    ota_fail("Update.begin failed (code %u)", (unsigned)Update.getError());
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
           "Downloading...");
  ::Serial.printf(
      "[OTA] Phase 3: Update.begin OK. Starting stream write...\r\n");

  WiFiClient *stream = http.getStreamPtr();
  if (!stream) {
    ota_fail("Failed to get stream pointer");
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  static uint8_t s_ota_buff[4096];
  size_t written = 0;
  uint32_t last_progress_time = millis();
  uint32_t download_start_ms = millis();
  uint32_t last_diag_ms = millis();
  int last_pct = -1;

  while (written < static_cast<size_t>(contentLength)) {
    esp_task_wdt_reset();

    if (millis() - download_start_ms > Config::OTA::DOWNLOAD_DEADLINE_MS) {
      ota_fail("Total download deadline exceeded");
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    if (millis() - last_progress_time > Config::OTA::STALL_TIMEOUT_MS) {
      ota_fail("Stream read stall timeout (25s)");
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    int avail = stream->available();
    if (!stream->connected() && avail == 0) {
      ota_fail("Connection lost prematurely at %u/%d bytes", (unsigned)written,
               contentLength);
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    if (avail <= 0) {
      vTaskDelay(pdMS_TO_TICKS(Config::OTA::IDLE_DELAY_MS));
      continue;
    }

    size_t to_read = (avail < static_cast<int>(sizeof(s_ota_buff)))
                         ? static_cast<size_t>(avail)
                         : sizeof(s_ota_buff);

    if (written + to_read > static_cast<size_t>(contentLength)) {
      to_read = static_cast<size_t>(contentLength) - written;
    }

    size_t read_bytes = stream->readBytes(s_ota_buff, to_read);
    if (read_bytes == 0) {
      vTaskDelay(pdMS_TO_TICKS(Config::OTA::IDLE_DELAY_MS));
      continue;
    }

    size_t written_bytes = Update.write(s_ota_buff, read_bytes);
    if (written_bytes != read_bytes) {
      ota_fail("Update.write mismatch (exp %u, got %u)", (unsigned)read_bytes,
               (unsigned)written_bytes);
      http.end();
      secure_client.stop();
      plain_client.stop();
      return false;
    }

    written += read_bytes;
    last_progress_time = millis();

    int pct = (written * 100) / contentLength;
    g_http_ota_state.progress_pct = pct;
    if (pct != last_pct && (pct % 10 == 0 || pct == 100)) {
      last_pct = pct;
      ::Serial.printf("[OTA] Progress: %d%% (%u / %d bytes)\r\n", pct,
                      (unsigned)written, contentLength);
    }

    if (TimeUtils::isElapsed(last_diag_ms, 5000)) {
      last_diag_ms = millis();
      uint32_t cur_free =
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      uint32_t cur_largest = heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      uint32_t cur_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                         MALLOC_CAP_8BIT);
      ::Serial.printf(
          "[OTA] %u/%d bytes (%d%%), free=%u, largest=%u, min=%u\r\n",
          (unsigned)written, contentLength, pct, (unsigned)cur_free,
          (unsigned)cur_largest, (unsigned)cur_min);
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }

  ::Serial.printf(
      "[OTA] Phase 4: Download complete (%u bytes). Finalizing update...\r\n",
      (unsigned)written);
  esp_task_wdt_reset();

  if (!Update.end()) {
    ota_fail("Update.end failed (code %u)", (unsigned)Update.getError());
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  if (!Update.isFinished()) {
    ota_fail("Update not finished");
    http.end();
    secure_client.stop();
    plain_client.stop();
    return false;
  }

  http.end();
  secure_client.stop();
  plain_client.stop();
  return true;
}

static bool do_ota(const char *initial_url) {
  OtaInProgressGuard ota_guard;

  WiFiClientSecure secure_client;
  WiFiClient plain_client;
  HTTPClient http;
  String final_url;
  int content_length = 0;

  if (!Ota_ResolveDownloadUrl(initial_url, final_url, secure_client,
                              plain_client, http, content_length)) {
    return false;
  }

  if (!Ota_StreamAndWritePartition(http, secure_client, plain_client,
                                   content_length)) {
    return false;
  }

  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status), "Success");
  g_http_ota_state.progress_pct = 100;
  ::Serial.printf(
      "[OTA] Firmware update SUCCESS! Rebooting in 1 second...\r\n");
  ota_guard.dismiss();
  return true;
}

static void Task_HttpOta(void *pvParameters) {
  // 진입 즉시 FreeRTOS WDT 감시 등록 (접속 단계 행 방지)
  bool wdt_registered = (esp_task_wdt_add(nullptr) == ESP_OK);

  const char *url = static_cast<const char *>(pvParameters);
  bool success = false;
  if (url && strlen(url) > 0) {
    success = do_ota(url);
  }

  if (!success) {
    if (wdt_registered) {
      esp_task_wdt_delete(nullptr);
    }
    Update.abort(); // 실패 시 OTA 파티션 언락
    // 모든 실패 경로는 여기서 단 한 번 안전하게 원복됨
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
    g_ota_in_progress.store(false, std::memory_order_release);
    g_http_ota_state.in_progress.store(false, std::memory_order_release);
    ::Serial.printf("[OTA] Task aborted. Stack high water mark: %u bytes\r\n",
                    (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    vTaskDelete(nullptr);
    return;
  }

  // 성공 경로: WDT 감시를 유지한 채 esp_task_wdt_reset() 호출 후 System_Restart
  // 실행 (System_Restart 내부의 NVS 쓰기나 CH5 락 블로킹 시 30초 WDT 패닉
  // 리부트로 자가 복구)
  ::Serial.printf("[OTA] Task complete. Stack high water mark: %u bytes\r\n",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  esp_task_wdt_reset();
  vTaskDelay(pdMS_TO_TICKS(1000));
  System_Restart("OTA Complete");

  // System_Restart가 반환되는 예외 상황에 대비한 복구 (정상 시 여기까지
  // 도달하지 않음)
  if (wdt_registered) {
    esp_task_wdt_delete(nullptr);
  }
  if (g_system_event_group) {
    xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
  }
  g_ota_in_progress.store(false, std::memory_order_release);
  g_http_ota_state.in_progress.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

class OtaAdmissionGuard {
public:
  OtaAdmissionGuard() {
    g_ota_in_progress.store(true, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
  }
  ~OtaAdmissionGuard() {
    if (!_dismissed) {
      g_ota_in_progress.store(false, std::memory_order_release);
      g_http_ota_state.in_progress.store(false, std::memory_order_release);
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
      }
    }
  }
  void dismiss() noexcept { _dismissed = true; }
  OtaAdmissionGuard(const OtaAdmissionGuard &) = delete;
  OtaAdmissionGuard &operator=(const OtaAdmissionGuard &) = delete;

private:
  bool _dismissed{false};
};

void System_StartHttpOta(const char *url) {
  // 1. 이미 진행 중인지 원자적으로 확인 (CAS)
  bool expected = false;
  if (!g_http_ota_state.in_progress.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    ::Serial.println("[OTA] Start requested but already in progress");
    return;
  }

  // 2. Admission Guard 생성 (모든 조기 리턴 및 실패 경로에서 자동 롤백 보장)
  OtaAdmissionGuard admission_guard;

  const char *target = url;
  if (!target || strlen(target) == 0) {
    target = DEFAULT_CLOUD_OTA_URL;
  }

  constexpr size_t MAX_INITIAL_URL_LEN = sizeof(s_ota_target_url) - 1;
  if (strlen(target) > MAX_INITIAL_URL_LEN) {
    ::Serial.printf("[OTA] Initial URL too long (%u bytes, max %u)\r\n",
                    (unsigned)strlen(target), (unsigned)MAX_INITIAL_URL_LEN);
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Initial OTA URL too long");
    return;
  }

  char target_host[128] = {0};
  char target_path[256] = {0};
  int target_port = 0;
  bool target_is_https = false;
  extract_url_components(target, target_host, sizeof(target_host), target_port,
                         target_path, sizeof(target_path), target_is_https);

  // 3. 외부 HTTPS인 경우 유효한 시스템 시간 Sanity check (NTP 미동기화 시
  // 인증서 검증 실패 방지)
  if (!is_private_host(target_host) && time(nullptr) < 1700000000) {
    ::Serial.println("[OTA] System time not synced (NTP required for TLS)");
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "System time not synced (NTP required)");
    return;
  }

  if (!is_trusted_ota_url(target, OtaUrlContext::Initial)) {
    ::Serial.printf("[OTA] Untrusted OTA URL: host=%s, port=%d, path=%s\r\n",
                    target_host[0] ? target_host : "invalid", target_port,
                    target_path);
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Untrusted OTA URL domain, port, or path");
    return;
  }

  // 4. 힙 메모리 여유 사전 검사 (TLS 핸드셰이크 최소 내부 SRAM 60KB & 연속 30KB
  // 확보)
  uint32_t free_heap =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t largest_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ::Serial.printf(
      "[OTA] Internal SRAM check: free=%u bytes, largest_block=%u bytes\r\n",
      (unsigned)free_heap, (unsigned)largest_block);

  if (free_heap < 60000 || largest_block < 30000) {
    ::Serial.printf("[OTA] Heap too low/fragmented: %u bytes (largest %u, need "
                    ">= 60000 / 30000)\r\n",
                    (unsigned)free_heap, (unsigned)largest_block);
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Heap too low (%u bytes, largest %u, need >= 60000)",
             (unsigned)free_heap, (unsigned)largest_block);
    return;
  }

  strncpy(s_ota_target_url, target, sizeof(s_ota_target_url) - 1);
  s_ota_target_url[sizeof(s_ota_target_url) - 1] = '\0';

  snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
           "Starting...");
  g_http_ota_state.progress_pct = 0;
  g_http_ota_state.last_error[0] = '\0';

  // 5. OTA 전용 백그라운드 태스크 생성 (Core 1, 우선순위 10, 스택 12KB)
  BaseType_t res = xTaskCreatePinnedToCore(Task_HttpOta, "HttpOtaTask", 12288,
                                           s_ota_target_url, 10, nullptr, 1);

  if (res != pdPASS) {
    ::Serial.printf("[OTA] Failed to create HttpOtaTask: %d\r\n", res);
    snprintf(g_http_ota_state.status, sizeof(g_http_ota_state.status),
             "Failed");
    snprintf(g_http_ota_state.last_error, sizeof(g_http_ota_state.last_error),
             "Failed to spawn OTA task");
    return;
  }

  // 6. 태스크 생성 성공: Task_HttpOta로 생명주기 인계
  admission_guard.dismiss();
}

void System_GetHttpOtaSnapshot(HttpOtaSnapshot &out) noexcept {
  out.in_progress = g_http_ota_state.in_progress.load(std::memory_order_relaxed);
  strncpy(out.status, g_http_ota_state.status, sizeof(out.status) - 1);
  out.status[sizeof(out.status) - 1] = '\0';
  out.progress_pct = g_http_ota_state.progress_pct;
  strncpy(out.last_error, g_http_ota_state.last_error, sizeof(out.last_error) - 1);
  out.last_error[sizeof(out.last_error) - 1] = '\0';
}

bool System_IsHttpOtaInProgress() noexcept {
  return g_http_ota_state.in_progress.load(std::memory_order_relaxed);
}

void SystemOta_InitArduinoOta(const char *hostname, const char *password) {
  ArduinoOTA.setHostname(hostname ? hostname : "gateway-bridge");
  if (password && password[0]) {
    ArduinoOTA.setPassword(password);
  }
  ArduinoOTA.onStart([]() {
    g_ota_in_progress.store(true, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
    ::Serial.println(F("[ArduinoOTA] Start transfer..."));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    esp_task_wdt_reset();
    System_FeedWdt(Config::Task::WDT_ID_NET);
  });
  ArduinoOTA.onEnd([]() {
    ::Serial.println(F("[ArduinoOTA] Finished successfully!"));
    System_Restart("OTA Firmware Update");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    g_ota_in_progress.store(false, std::memory_order_release);
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_OTA_IDLE);
    }
    ::Serial.printf("[ArduinoOTA] Error (%u)\r\n", (unsigned)error);
  });
  ArduinoOTA.begin();
}


