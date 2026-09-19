#include "CliCommon.h"

namespace ConfigCli {

enum ParamType {
  PARAM_UINT32,
  PARAM_UINT16,
  PARAM_UCHAR,
  PARAM_STRING,
  PARAM_PASS_HASH
};

struct ConfigParamDef {
  const char *name;
  ParamType type;
  union {
    uint32_t *u32;
    uint16_t *u16;
    uint8_t *u8;
    char *str;
  } ptr;
  size_t maxLen;
};

static const ConfigParamDef PARAM_TABLE[] = {
    {"uart_baud", PARAM_UINT32, {.u32 = &g_config.uart_baud_rate}, 0},
    {"uart_databits", PARAM_UCHAR, {.u8 = &g_config.uart_data_bits}, 0},
    {"uart_parity", PARAM_UCHAR, {.u8 = &g_config.uart_parity}, 0},
    {"uart_stopbits", PARAM_UCHAR, {.u8 = &g_config.uart_stop_bits}, 0},
    {"door_baud", PARAM_UINT32, {.u32 = &g_config.doorphone_baud_rate}, 0},
    {"door_databits", PARAM_UCHAR, {.u8 = &g_config.doorphone_data_bits}, 0},
    {"door_parity", PARAM_UCHAR, {.u8 = &g_config.doorphone_parity}, 0},
    {"door_stopbits", PARAM_UCHAR, {.u8 = &g_config.doorphone_stop_bits}, 0},
    {"wifi_ssid",
     PARAM_STRING,
     {.str = g_config.wifi_ssid},
     sizeof(g_config.wifi_ssid)},
    {"wifi_pass",
     PARAM_STRING,
     {.str = g_config.wifi_password},
     sizeof(g_config.wifi_password)},
    {"ap_ssid",
     PARAM_STRING,
     {.str = g_config.ap_ssid},
     sizeof(g_config.ap_ssid)},
    {"ap_pass",
     PARAM_STRING,
     {.str = g_config.ap_password},
     sizeof(g_config.ap_password)},
    {"wifi_timeout",
     PARAM_UINT16,
     {.u16 = &g_config.wifi_connect_timeout_s},
     0},
    {"telnet_pass", PARAM_PASS_HASH, {.str = g_config.telnet_pass_hash}, 0},
};
static const size_t PARAM_COUNT = sizeof(PARAM_TABLE) / sizeof(ConfigParamDef);

void printConfig(int sock) {
  char buf[1024], fu[10], fd[10];
  auto formatFraming = [](uint8_t d, uint8_t p, uint8_t s, char *o) {
    snprintf(o, 10, "%u%c%u", d, p == 1 ? 'E' : p == 2 ? 'O' : 'N', s);
  };
  formatFraming(g_config.uart_data_bits, g_config.uart_parity,
                g_config.uart_stop_bits, fu);
  formatFraming(g_config.doorphone_data_bits, g_config.doorphone_parity,
                g_config.doorphone_stop_bits, fd);

  snprintf(buf, sizeof(buf),
           "\r\n================================================================"
           "================\r\n"
           "                         GATEWAY CONFIGURATION (NVS Flash)      "
           "        \r\n"
           "================================================================"
           "================\r\n"
           "WiFi Station SSID : %s\r\n"
           "WiFi SoftAP SSID  : %s\r\n"
           "WiFi Connect Tout : %u sec\r\n"
           "Wallpad Profile   : ID %u\r\n"
           "UART RS-485 Conf  : %u bps, %s\r\n"
           "Doorphone UART    : %u bps, %s\r\n"
           "================================================================"
           "================\r\n\r\n",
           g_config.wifi_ssid, g_config.ap_ssid,
           g_config.wifi_connect_timeout_s,
           static_cast<unsigned>(g_config.wallpad_profile),
           static_cast<unsigned>(g_config.uart_baud_rate), fu,
           static_cast<unsigned>(g_config.doorphone_baud_rate), fd);
  sendTelnetMsg(sock, buf);
}

template <typename T>
static bool applyUintParam(T *dest, const char *value, unsigned long max_val,
                           int sock, const char *key) {
  char *endp = nullptr;
  unsigned long v = strtoul(value, &endp, 10);
  if (!endp || *endp != '\0' || v > max_val) {
    sendTelnetMsgf(sock, "[ERROR] Invalid value for '%s' (max: %lu)\r\n", key, max_val);
    return false;
  }
  *dest = static_cast<T>(v);
  g_config_dirty.store(true, std::memory_order_relaxed);
  sendTelnetMsgf(sock, "[OK] Set %s = %lu (Not saved to NVS yet. Use 'save' to commit)\r\n", key, v);
  return true;
}

void setConfig(void *session_context, const char *key, const char *value) {
  int sock = getSock(session_context);
  if (!key || !value) {
    sendTelnetMsg(sock, "[ERROR] Usage: config set <key> <value>\r\n");
    return;
  }

  for (size_t i = 0; i < PARAM_COUNT; ++i) {
    if (strcasecmp(PARAM_TABLE[i].name, key) == 0) {
      CriticalSectionLocker lock(&g_config_mux);
      switch (PARAM_TABLE[i].type) {
      case PARAM_UINT32:
        applyUintParam(PARAM_TABLE[i].ptr.u32, value, UINT32_MAX, sock, key);
        return;
      case PARAM_UINT16:
        applyUintParam(PARAM_TABLE[i].ptr.u16, value, 65535UL, sock, key);
        return;
      case PARAM_UCHAR:
        applyUintParam(PARAM_TABLE[i].ptr.u8, value, 255UL, sock, key);
        return;
      case PARAM_STRING: {
        if (strlen(value) < PARAM_TABLE[i].maxLen) {
          strncpy(PARAM_TABLE[i].ptr.str, value, PARAM_TABLE[i].maxLen - 1);
          PARAM_TABLE[i].ptr.str[PARAM_TABLE[i].maxLen - 1] = '\0';
          g_config_dirty.store(true, std::memory_order_relaxed);
          sendTelnetMsgf(sock, "[OK] Set %s = '%s' (Not saved to NVS yet. Use 'save' to commit)\r\n", key, value);
        } else {
          sendTelnetMsg(sock, "[ERROR] String exceeds maximum allowed length.\r\n");
        }
        return;
      }
      case PARAM_PASS_HASH: {
        char hash_hex[68];
        System_Sha256ToHex(value, hash_hex);
        strncpy(g_config.telnet_pass_hash, hash_hex, sizeof(g_config.telnet_pass_hash) - 1);
        g_config.telnet_pass_hash[sizeof(g_config.telnet_pass_hash) - 1] = '\0';
        g_config_dirty.store(true, std::memory_order_relaxed);
        sendTelnetMsg(sock, "[OK] Telnet password updated & SHA-256 hashed. Use 'save' to make permanent.\r\n");
        return;
      }
      }
    }
  }

  sendTelnetMsgf(sock, "[ERROR] Unknown configuration parameter '%s'. Type 'config' to see all params.\r\n", key);
}

void cmdConfig(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc == 0) {
    printConfig(sock);
    return;
  }

  const char *sub = embeddedCliGetToken(args, 1);
  if (strcasecmp(sub, "set") == 0) {
    if (argc >= 3) {
      setConfig(context, embeddedCliGetToken(args, 2),
                embeddedCliGetToken(args, 3));
    } else {
      sendTelnetMsg(sock, "[ERROR] Usage: config set <key> <value>\r\n");
    }
    return;
  }

  if (strcasecmp(sub, "reset") == 0) {
    Config_ResetDefaults();
    sendTelnetMsg(sock, "[OK] Runtime configuration reset to system factory defaults. (Not saved to NVS)\r\n");
    return;
  }

  sendTelnetMsg(sock, "Usage: config [set <key> <value> | reset]\r\n");
}

void cmdSave(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  Config_Save();
  sendTelnetMsg(sock, "[OK] Configuration successfully committed and saved to NVS flash!\r\n");
}

void cmdEw11(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc == 0 || (argc == 1 && strcasecmp(embeddedCliGetToken(args, 1), "list") == 0) ||
      (argc == 1 && strcasecmp(embeddedCliGetToken(args, 1), "status") == 0)) {
    char buf[1024];
    AppendBuf out{buf, sizeof(buf)};
    out.append("\r\n");
    out.append(Fmt::DIV80EQ);
    out.append("                      CH5 EW11 TCP CLIENT SOCKET STATUS                         \r\n");
    out.append(Fmt::DIV80EQ);
    out.appendFormat("%-6s %-13s %-6s %-18s %-14s %s\r\n",
                     "Slot", "Name", "Port", "Client IP", "Status", "Packets(RX/TX)");
    out.append(Fmt::DIV80);

    {
      MutexLocker lock(g_ch5_mutex);
      for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
        auto &slot = g_hub_slots[s];
        const char *status_str = !slot.enabled ? "Disabled"
                                 : !slot.is_connected ? "Listening"
                                 : (slot.rx_pkts == 0) ? "Idle" : "Connected";

        out.appendFormat("#%-5d %-13s %-6u %-18s %-14s %u / %u\r\n",
                         s, slot.name,
                         slot.target_port,
                         slot.is_connected ? (slot.target_ip[0] ? slot.target_ip : "Connected") : (slot.target_ip[0] ? slot.target_ip : "-"),
                         status_str,
                         static_cast<unsigned>(slot.rx_pkts),
                         static_cast<unsigned>(slot.tx_pkts));
      }
    }
    out.append(Fmt::DIV80);
    sendTelnetMsgLen(sock, out.buf, out.offset);
    return;
  }

  const char *sub = embeddedCliGetToken(args, 1);

  if (strcasecmp(sub, "frame") == 0) {
    if (argc < 4) {
      sendTelnetMsg(sock, "[ERROR] Usage: ew11 frame <slot:0-4> <stx:hex> <etx:hex> [len:dec]\r\n");
      return;
    }
    int slot = atoi(embeddedCliGetToken(args, 2));
    if (slot < 0 || slot >= Config::TCP::MAX_EW11_SLOTS) {
      sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n", Config::TCP::MAX_EW11_SLOTS - 1);
      return;
    }
    uint8_t stx = static_cast<uint8_t>(strtoul(embeddedCliGetToken(args, 3), nullptr, 16));
    uint8_t etx = static_cast<uint8_t>(strtoul(embeddedCliGetToken(args, 4), nullptr, 16));
    uint8_t len = 0;
    if (argc >= 5) {
      len = static_cast<uint8_t>(atoi(embeddedCliGetToken(args, 5)));
    }
    char ns[16], tag[16];
    snprintf(ns, sizeof(ns), "e%d_frame", slot);
    snprintf(tag, sizeof(tag), "EW11_#%d", slot);
    g_hub_slots[slot].tracker.setFixedLock(stx, etx, len);
    g_hub_slots[slot].tracker.saveToNvs(ns, tag);
    sendTelnetMsgf(sock, "[OK] EW11 Slot #%d framing permanently fixed to STX 0x%02X, ETX 0x%02X, Len %u.\r\n",
                   slot, stx, etx, len);
    return;
  }

  if (strcasecmp(sub, "reset") == 0) {
    if (argc < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: ew11 reset <slot:0-4>\r\n");
      return;
    }
    int slot = atoi(embeddedCliGetToken(args, 2));
    if (slot < 0 || slot >= Config::TCP::MAX_EW11_SLOTS) {
      sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n", Config::TCP::MAX_EW11_SLOTS - 1);
      return;
    }
    char ns[16], tag[16];
    snprintf(ns, sizeof(ns), "e%d_frame", slot);
    snprintf(tag, sizeof(tag), "EW11_#%d", slot);
    g_hub_slots[slot].tracker.clearNvs(ns, tag);
    sendTelnetMsgf(sock, "[OK] EW11 Slot #%d framing tracker reset to autonomous auto-probing.\r\n", slot);
    return;
  }

  if (strcasecmp(sub, "set") == 0) {
    if (argc < 2) {
      sendTelnetMsg(sock, "[ERROR] Usage: ew11 set <slot:0-4> [port] [allowed_ip] [name] [enable:1/0]\r\n");
      return;
    }
    int slot = atoi(embeddedCliGetToken(args, 2));
    if (slot < 0 || slot >= Config::TCP::MAX_EW11_SLOTS) {
      sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n", Config::TCP::MAX_EW11_SLOTS - 1);
      return;
    }

    uint16_t default_port = Config::TCP::EW11_SLOT_PORTS[slot];
    uint16_t port = g_hub_slots[slot].target_port > 0 ? g_hub_slots[slot].target_port : default_port;
    const char *ip_str = nullptr;
    const char *name_str = nullptr;
    bool enabled = g_hub_slots[slot].enabled;

    // 인자 파싱: ew11 set <slot> [port/ip] ...
    // 토큰 3이 숫자(포트)인지 IP인지 유연하게 지원
    if (argc >= 3) {
      const char *tok3 = embeddedCliGetToken(args, 3);
      if (strchr(tok3, '.') != nullptr) {
        // IP 주소 형식인 경우
        ip_str = (strcmp(tok3, "-") == 0 || strcmp(tok3, "none") == 0) ? "" : tok3;
      } else {
        int p_val = atoi(tok3);
        if (p_val > 0 && p_val <= 65535) port = static_cast<uint16_t>(p_val);
      }
    }

    if (argc >= 4) {
      const char *tok4 = embeddedCliGetToken(args, 4);
      if (strchr(tok4, '.') != nullptr) {
        ip_str = (strcmp(tok4, "-") == 0 || strcmp(tok4, "none") == 0) ? "" : tok4;
      } else if (port == default_port && atoi(tok4) > 0) {
        port = static_cast<uint16_t>(atoi(tok4));
      } else {
        name_str = tok4;
      }
    }

    if (argc >= 5) {
      const char *tok5 = embeddedCliGetToken(args, 5);
      if (name_str == nullptr && !isdigit(tok5[0])) {
        name_str = tok5;
      } else if (isdigit(tok5[0])) {
        enabled = (atoi(tok5) != 0);
      }
    }

    if (argc >= 6) {
      enabled = (atoi(embeddedCliGetToken(args, 6)) != 0);
    }

    if (Hub_SetSlot(static_cast<uint8_t>(slot), enabled, ip_str, port, name_str)) {
      sendTelnetMsgf(sock, "[OK] EW11 Slot #%d configured (Name: %s, Listen Port: %u, Allowed IP: %s, Enabled: %s) and saved to NVS!\r\n",
                     slot, g_hub_slots[slot].name, g_hub_slots[slot].target_port,
                     g_hub_slots[slot].target_ip[0] ? g_hub_slots[slot].target_ip : "Any",
                     g_hub_slots[slot].enabled ? "true" : "false");
    } else {
      sendTelnetMsg(sock, "[ERROR] Failed to configure EW11 slot.\r\n");
    }
    return;
  }

  if (strcasecmp(sub, "enable") == 0 || strcasecmp(sub, "disable") == 0) {
    if (argc < 2) {
      sendTelnetMsgf(sock, "[ERROR] Usage: ew11 %s <slot:0-4>\r\n", sub);
      return;
    }
    int slot = atoi(embeddedCliGetToken(args, 2));
    if (slot < 0 || slot >= Config::TCP::MAX_EW11_SLOTS) {
      sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n", Config::TCP::MAX_EW11_SLOTS - 1);
      return;
    }
    bool enable = (strcasecmp(sub, "enable") == 0);
    {
      MutexLocker lock(g_ch5_mutex);
      g_hub_slots[slot].enabled = enable;
      if (!enable && g_hub_slots[slot].sock >= 0) {
        close(g_hub_slots[slot].sock);
        g_hub_slots[slot].sock = -1;
        g_hub_slots[slot].is_connected = false;
        g_hub_slots[slot].rx_len = 0;
      }
    }
    Hub_SaveConfig();
    sendTelnetMsgf(sock, "[OK] EW11 Slot #%d %s and saved to NVS flash.\r\n", slot, enable ? "ENABLED" : "DISABLED");
    return;
  }

  sendTelnetMsg(sock, "Usage: ew11 [list | set <slot> [port] [allowed_ip] [name] [enable] | frame <slot> <stx> <etx> [len] | reset <slot> | enable <slot> | disable <slot>]\r\n");
}

void cmdRoutes(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc == 1 && strcasecmp(embeddedCliGetToken(args, 1), "clear") == 0) {
    g_route_registry.clear();
    sendTelnetMsg(sock, "[OK] Dynamic device ingress routing table cleared.\r\n");
    return;
  }

  static DeviceRouteEntry entries[DeviceRouteRegistry::MAX_ROUTES];
  size_t count = g_route_registry.getRoutes(entries, DeviceRouteRegistry::MAX_ROUTES);

  char buf[2048];
  AppendBuf out{buf, sizeof(buf)};
  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                 DYNAMIC DEVICE INGRESS ROUTING TABLE (Zero Hardcode)         \r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat("%-22s %-25s %s\r\n", "Target [DevID:Sub1:Sub2]", "Egress Destination", "Last Seen");
  out.append(Fmt::DIV80);

  if (count == 0) {
    out.append("  (No device routes learned yet. Waiting for bus/EW11 packets...)\r\n");
  } else {
    uint32_t now = millis();
    for (size_t i = 0; i < count; i++) {
      const auto &e = entries[i];
      char tgt_str[24];
      snprintf(tgt_str, sizeof(tgt_str), "[0x%02X:%02X:%02X]", e.dev_id, e.sub1, e.sub2);

      char dst_str[32];
      if (e.endpoint.channel_id == 5 && e.endpoint.slot_idx >= 0) {
        snprintf(dst_str, sizeof(dst_str), "CH#5 Slot %d", e.endpoint.slot_idx);
      } else {
        snprintf(dst_str, sizeof(dst_str), "CH#%u", e.endpoint.channel_id);
      }

      char el_str[20];
      Fmt::FormatElapsed(now, e.endpoint.last_seen_ms, el_str, sizeof(el_str));

      out.appendFormat("  %-20s -> %-23s (%s ago)\r\n", tgt_str, dst_str, el_str);
    }
  }

  out.append(Fmt::DIV80);
  out.append("Usage: routes        - View learned device routes\r\n");
  out.append("       routes clear  - Clear routing table cache\r\n\r\n");
  sendTelnetMsgLen(sock, out.buf, out.offset);
}

} // namespace ConfigCli
