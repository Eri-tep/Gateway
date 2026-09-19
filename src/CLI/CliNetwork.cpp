#include "CliCommon.h"
#include <WiFi.h>

namespace WifiCli {

static void AsyncWifiScanTask(void *pvParameters) {
  if (!pvParameters) {
    vTaskDelete(nullptr);
    return;
  }
  TelnetManager::WifiScanReq req = *static_cast<TelnetManager::WifiScanReq *>(pvParameters);
  vTaskDelay(pdMS_TO_TICKS(100));

  int n = WiFi.scanNetworks(false, true);

  auto scan_buf = std::make_unique<char[]>(4096);
  scan_buf[0] = '\0';
  AppendBuf out{scan_buf.get(), 4096};

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                      2.4GHz WI-FI ACCESS POINT SCAN RESULTS                  \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("No   SSID                             RSSI      Channel  Encryption\r\n");
  out.append(Fmt::DIV80);

  if (n == 0) {
    out.append("  (No wireless networks found)\r\n");
  } else if (n < 0) {
    out.append("  [ERROR] Wi-Fi hardware scan failed or timed out.\r\n");
  } else {
    for (int i = 0; i < n; ++i) {
      const char *encType = "Open";
      switch (WiFi.encryptionType(i)) {
      case WIFI_AUTH_WEP: encType = "WEP"; break;
      case WIFI_AUTH_WPA_PSK: encType = "WPA"; break;
      case WIFI_AUTH_WPA2_PSK: encType = "WPA2"; break;
      case WIFI_AUTH_WPA_WPA2_PSK: encType = "WPA/WPA2"; break;
      case WIFI_AUTH_WPA2_ENTERPRISE: encType = "Enterprise"; break;
      case WIFI_AUTH_WPA3_PSK: encType = "WPA3"; break;
      case WIFI_AUTH_WPA2_WPA3_PSK: encType = "WPA2/WPA3"; break;
      default: break;
      }
      out.appendFormat("%02d   %-32s %4d dBm   %3d      %s\r\n", i + 1,
                       WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                       encType);
    }
  }
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
  WiFi.scanDelete();

  g_telnet_manager.sendScanResult(req, out.buf);
  vTaskDelete(nullptr);
}

void cmdWifi(EmbeddedCli *cli, char *args, void *context) {
  auto *sess = getSession(context);
  int sock = sess ? sess->sock : -1;
  uint8_t count = embeddedCliGetTokenCount(args);
  const char *subCmd = (count > 0) ? embeddedCliGetToken(args, 1) : "status";

  if (count == 0 || strcasecmp(subCmd, "status") == 0) {
    g_cli_scratch_buf[0] = '\0';
    AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};

    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("                      WI-FI HARDWARE & NETWORK STATUS                         \r\n");
    out.append(Fmt::DIV80EQ);
    out.append("Category            Parameter       Value / Target                        Status\r\n");
    out.append(Fmt::DIV80);

    bool sta_ok = (WiFi.status() == WL_CONNECTED);
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "Station (STA)", "SSID",
                     sta_ok ? WiFi.SSID().c_str() : g_config.wifi_ssid,
                     sta_ok ? "[CONNECTED]" : "[DISCONNECTED]");
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "IP Address",
                     sta_ok ? WiFi.localIP().toString().c_str() : "0.0.0.0",
                     sta_ok ? "[ACTIVE]" : "[IDLE]");
    char rssi_b[16];
    snprintf(rssi_b, sizeof(rssi_b), "%d dBm", WiFi.RSSI());
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "Signal (RSSI)",
                     sta_ok ? rssi_b : "N/A", sta_ok ? "[STABLE]" : "[IDLE]");
    out.append(Fmt::DIV80);

    bool ap_active = (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA);
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "SoftAP (AP)", "SSID",
                     g_config.ap_ssid, ap_active ? "[BROADCASTING]" : "[DISABLED]");
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "AP IP",
                     ap_active ? WiFi.softAPIP().toString().c_str() : "0.0.0.0",
                     ap_active ? "[ACTIVE]" : "[INACTIVE]");
    out.appendFormat("%-20s%-16s%-32s%12s\r\n", "", "Clients",
                     ap_active ? "Max 4 Clients" : "0 Clients",
                     ap_active ? "[READY]" : "[OFF]");
    out.append(Fmt::DIV80EQ);
    out.append("\r\n");

    sendTelnetMsgLen(sock, out.buf, out.offset);
    return;
  }

  if (strcasecmp(subCmd, "scan") == 0) {
    sendTelnetMsg(sock, "[WIFI] Scanning background 2.4GHz APs (Takes 2-3s)...\r\n");
    g_wifi_scan_req.clientIp = sess ? sess->clientIp : IPAddress(0, 0, 0, 0);
    g_wifi_scan_req.sessionId = sess ? sess->sessionId : 0;
    xTaskCreatePinnedToCore(
        AsyncWifiScanTask, "WifiScanWorker", 4096, &g_wifi_scan_req, 2,
        NULL, 0);
  } else if (strcasecmp(subCmd, "connect") == 0) {
    if (count < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: wifi connect <ssid> [password]\r\n");
      return;
    }
    const char *ssid_arg = embeddedCliGetToken(args, 2);
    const char *pass_arg = (count >= 3) ? embeddedCliGetToken(args, 3) : "";

    {
      CriticalSectionLocker lock(&g_config_mux);
      strncpy(g_config.wifi_ssid, ssid_arg, sizeof(g_config.wifi_ssid) - 1);
      g_config.wifi_ssid[sizeof(g_config.wifi_ssid) - 1] = '\0';
      strncpy(g_config.wifi_password, pass_arg, sizeof(g_config.wifi_password) - 1);
      g_config.wifi_password[sizeof(g_config.wifi_password) - 1] = '\0';
    }
    Config_Save();

    sendTelnetMsgf(sock, "[WIFI] Saved SSID '%s' to NVS. Connecting...\r\n", ssid_arg);
    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
  } else if (strcasecmp(subCmd, "disconnect") == 0) {
    WiFi.disconnect(false);
    sendTelnetMsg(sock, "[WIFI] Disconnected from Wi-Fi AP.\r\n");
  } else {
    sendTelnetMsg(sock, "Usage: wifi [status | scan | connect <ssid> [password] | disconnect]\r\n");
  }
}

} // namespace WifiCli
