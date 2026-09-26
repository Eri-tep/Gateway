#include "CliCommon.h"
#include "MgmtRpc.h"

namespace ConfigCli {

enum ParamType {
  PARAM_UINT32,
  PARAM_UINT16,
  PARAM_UCHAR,
  PARAM_STRING,
  PARAM_PASS_HASH,
  PARAM_FRAMING_CH1,
  PARAM_FRAMING_CH2,
  PARAM_FRAMING_CH3,
  PARAM_FRAMING_CH4,
  PARAM_TIMING_CH1,
  PARAM_TIMING_CH2,
  PARAM_TIMING_CH3
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
  uint32_t minVal;
  uint32_t maxVal;
  const char *desc;
};

static const ConfigParamDef PARAM_TABLE[] = {
    // Channel Baudrates
    {"ch1_baud", PARAM_UINT32, {.u32 = &g_config.uart_baud_rate}, 1200, 115200, "CH1 Device Master Baudrate (bps)"},
    {"ch2_baud", PARAM_UINT32, {.u32 = &g_config.ch2_baud_rate}, 1200, 115200, "CH2 Main Wallpad Baudrate (bps)"},
    {"ch3_baud", PARAM_UINT32, {.u32 = &g_config.ch3_baud_rate}, 1200, 115200, "CH3 Sub Wallpad Baudrate (bps)"},
    {"ch4_baud", PARAM_UINT32, {.u32 = &g_config.doorphone_baud_rate}, 1200, 115200, "CH4 Doorphone Baudrate (bps)"},

    // Channel Framings (8N1, 8E1, 8O1, 8N2)
    {"ch1_framing", PARAM_FRAMING_CH1, {.u8 = nullptr}, 0, 0, "CH1 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch2_framing", PARAM_FRAMING_CH2, {.u8 = nullptr}, 0, 0, "CH2 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch3_framing", PARAM_FRAMING_CH3, {.u8 = nullptr}, 0, 0, "CH3 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch4_framing", PARAM_FRAMING_CH4, {.u8 = nullptr}, 0, 0, "CH4 Framing (8N1, 8E1, 8O1, 8N2)"},

    // Channel Delays
    {"ch1_poll", PARAM_TIMING_CH1, {.u16 = &g_timing_config.ch1_poll_interval_ms}, 200, 5000, "CH1 Master Polling Interval (ms)"},
    {"ch2_ack",  PARAM_TIMING_CH2, {.u16 = &g_timing_config.ch2_cache_delay_ms},   5,   300,  "CH2 Main Wallpad Virtual ACK (ms)"},
    {"ch3_ack",  PARAM_TIMING_CH3, {.u16 = &g_timing_config.ch3_cache_delay_ms},   20,  1000, "CH3 Sub Wallpad Virtual ACK (ms)"},

    // Wallpad Profile
    {"profile", PARAM_UCHAR, {.u8 = &g_config.wallpad_profile}, 0, 3, "Wallpad Profile Slot (0=Auto, 1=Custom1, etc)"},

    // Wi-Fi & Network
    {"wifi_ssid", PARAM_STRING, {.str = g_config.wifi_ssid}, 0, sizeof(g_config.wifi_ssid) - 1, "Station Wi-Fi SSID"},
    {"wifi_pass", PARAM_STRING, {.str = g_config.wifi_password}, 0, sizeof(g_config.wifi_password) - 1, "Station Wi-Fi Password"},
    {"ap_ssid",   PARAM_STRING, {.str = g_config.ap_ssid}, 0, sizeof(g_config.ap_ssid) - 1, "SoftAP SSID"},
    {"ap_pass",   PARAM_STRING, {.str = g_config.ap_password}, 0, sizeof(g_config.ap_password) - 1, "SoftAP Password"},
    {"wifi_timeout", PARAM_UINT16, {.u16 = &g_config.wifi_connect_timeout_s}, 5, 120, "Wi-Fi Connection Timeout (seconds)"},

    // Security
    {"telnet_pass", PARAM_PASS_HASH, {.str = g_config.telnet_pass_hash}, 0, 0, "Telnet Login Password"},
};
static const size_t PARAM_COUNT = sizeof(PARAM_TABLE) / sizeof(ConfigParamDef);

void printConfig(int sock) {
  char buf[2048];
  AppendBuf out{buf, sizeof(buf)};

  char f1[8], f2[8], f3[8], f4[8];
  strncpy(f1, formatFramingStr(g_config.uart_data_bits, g_config.uart_parity, g_config.uart_stop_bits), sizeof(f1));
  strncpy(f2, formatFramingStr(g_config.ch2_data_bits, g_config.ch2_parity, g_config.ch2_stop_bits), sizeof(f2));
  strncpy(f3, formatFramingStr(g_config.ch3_data_bits, g_config.ch3_parity, g_config.ch3_stop_bits), sizeof(f3));
  strncpy(f4, formatFramingStr(g_config.doorphone_data_bits, g_config.doorphone_parity, g_config.doorphone_stop_bits), sizeof(f4));

  const char *prof_name = "Auto Detect (Slot 0)";
  if (g_config.wallpad_profile == 1) prof_name = "Custom Slot 1";
  else if (g_config.wallpad_profile == 2) prof_name = "Custom Slot 2";
  else if (g_config.wallpad_profile == 3) prof_name = "Custom Slot 3";

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                         GATEWAY CONFIGURATION (NVS Flash)              \r\n");
  out.append(Fmt::DIV80EQ);
  out.append("[Network & Security]\r\n");
  out.appendFormat("  WiFi Station SSID : %s\r\n", g_config.wifi_ssid[0] ? g_config.wifi_ssid : "(Not Configured)");
  out.appendFormat("  WiFi SoftAP SSID  : %s\r\n", g_config.ap_ssid[0] ? g_config.ap_ssid : "(Disabled)");
  out.appendFormat("  WiFi Connect Tout : %u sec\r\n", g_config.wifi_connect_timeout_s);
  out.appendFormat("  Telnet Password   : %s\r\n", g_config.telnet_pass_hash[0] ? "Configured (SHA-256)" : "Default (None)");
  out.append("\r\n");
  out.append("[Wallpad & Protocol]\r\n");
  out.appendFormat("  Wallpad Profile   : %s\r\n", prof_name);
  out.append("\r\n");
  out.append("[Channels & RS-485 / Delays]\r\n");
  out.appendFormat("  CH1 (Device Master) : %-6u bps, %-3s | Query Interval : %u ms\r\n",
                   static_cast<unsigned>(g_config.uart_baud_rate), f1, g_timing_config.ch1_poll_interval_ms);
  out.appendFormat("  CH2 (Main Wallpad)  : %-6u bps, %-3s | Virtual ACK    : %u ms\r\n",
                   static_cast<unsigned>(g_config.ch2_baud_rate), f2, g_timing_config.ch2_cache_delay_ms);
  out.appendFormat("  CH3 (Sub Wallpad)   : %-6u bps, %-3s | Virtual ACK    : %u ms\r\n",
                   static_cast<unsigned>(g_config.ch3_baud_rate), f3, g_timing_config.ch3_cache_delay_ms);
  out.appendFormat("  CH4 (Doorphone)     : %-6u bps, %-3s | RX/TX Isolated\r\n",
                   static_cast<unsigned>(g_config.doorphone_baud_rate), f4);
  out.append(Fmt::DIV80EQ);
  out.append("Type 'config ?' or 'config help' to view all configurable parameter keys.\r\n\r\n");

  sendTelnetMsgLen(sock, out.buf, out.offset);
}

void printConfigHelp(int sock) {
  char buf[2048];
  AppendBuf out{buf, sizeof(buf)};

  out.append("\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("                         CONFIGURABLE PARAMETERS GUIDE                          \r\n");
  out.append(Fmt::DIV80EQ);
  out.appendFormat("%-14s %-16s %s\r\n", "Parameter Key", "Valid Range / Type", "Description");
  out.append(Fmt::DIV80);

  for (size_t i = 0; i < PARAM_COUNT; ++i) {
    const auto &p = PARAM_TABLE[i];
    char range_buf[24];
    if (p.type == PARAM_UINT32 || p.type == PARAM_UINT16 || p.type == PARAM_UCHAR ||
        p.type == PARAM_TIMING_CH1 || p.type == PARAM_TIMING_CH2 || p.type == PARAM_TIMING_CH3) {
      snprintf(range_buf, sizeof(range_buf), "%lu ~ %lu", (unsigned long)p.minVal, (unsigned long)p.maxVal);
    } else if (p.type >= PARAM_FRAMING_CH1 && p.type <= PARAM_FRAMING_CH4) {
      snprintf(range_buf, sizeof(range_buf), "8N1,8E1,8O1,8N2");
    } else if (p.type == PARAM_PASS_HASH) {
      snprintf(range_buf, sizeof(range_buf), "string (raw)");
    } else {
      snprintf(range_buf, sizeof(range_buf), "string");
    }

    out.appendFormat("%-14s %-18s %s\r\n", p.name, range_buf, p.desc);
  }

  out.append(Fmt::DIV80);
  out.append("Usage:\r\n");
  out.append("  config set <key> <value>   : Modify parameter (RAM only)\r\n");
  out.append("  save                       : Commit modified parameters to NVS flash permanently\r\n");
  out.append("  config reset               : Restore all configuration to factory defaults\r\n");
  out.append(Fmt::DIV80EQ);
  out.append("\r\n");

  sendTelnetMsgLen(sock, out.buf, out.offset);
}

template <typename T>
static bool applyUintParam(T *dest, const char *value, unsigned long min_val, unsigned long max_val,
                           int sock, const char *key) {
  char *endp = nullptr;
  unsigned long v = strtoul(value, &endp, 10);
  if (!endp || *endp != '\0' || v < min_val || v > max_val) {
    sendTelnetMsgf(sock, "[ERROR] Invalid value '%s' for '%s' (Allowed: %lu ~ %lu)\r\n", value, key, min_val, max_val);
    return false;
  }
  *dest = static_cast<T>(v);
  g_config_dirty.store(true, std::memory_order_relaxed);
  sendTelnetMsgf(sock, "[OK] Set %s = %lu (RAM only. Use 'save' to commit to NVS)\r\n", key, v);
  return true;
}

static bool applyFraming(uint8_t &dbits, uint8_t &parity, uint8_t &sbits, const char *value, int sock, const char *key) {
  uint8_t d, p, s;
  if (!parseFramingStr(value, d, p, s)) {
    sendTelnetMsgf(sock, "[ERROR] Invalid framing '%s'. Choose from: 8N1, 8E1, 8O1, 8N2\r\n", value);
    return false;
  }
  dbits = d;
  parity = p;
  sbits = s;
  g_config_dirty.store(true, std::memory_order_relaxed);
  sendTelnetMsgf(sock, "[OK] Set %s = '%s' (RAM only. Use 'save' to commit to NVS)\r\n", key, value);
  return true;
}

void setConfig(void *session_context, const char *key, const char *value) {
  int sock = getSock(session_context);
  if (!key || !value) {
    sendTelnetMsg(sock, "[ERROR] Usage: config set <key> <value>\r\n");
    return;
  }

  for (size_t i = 0; i < PARAM_COUNT; ++i) {
    const auto &p = PARAM_TABLE[i];
    if (strcasecmp(p.name, key) == 0) {
      CriticalSectionLocker lock(&g_config_mux);
      switch (p.type) {
      case PARAM_UINT32:
        applyUintParam(p.ptr.u32, value, p.minVal, p.maxVal, sock, key);
        return;
      case PARAM_UINT16:
        applyUintParam(p.ptr.u16, value, p.minVal, p.maxVal, sock, key);
        return;
      case PARAM_UCHAR:
        applyUintParam(p.ptr.u8, value, p.minVal, p.maxVal, sock, key);
        return;
      case PARAM_TIMING_CH1:
      case PARAM_TIMING_CH2:
      case PARAM_TIMING_CH3: {
        char *endp = nullptr;
        unsigned long v = strtoul(value, &endp, 10);
        if (!endp || *endp != '\0' || v < p.minVal || v > p.maxVal) {
          sendTelnetMsgf(sock, "[ERROR] Invalid delay '%s' for '%s' (Allowed: %lu ~ %lu ms)\r\n", value, key, (unsigned long)p.minVal, (unsigned long)p.maxVal);
          return;
        }
        *p.ptr.u16 = static_cast<uint16_t>(v);
        TimingConfig_Save();
        sendTelnetMsgf(sock, "[OK] Set %s = %lu ms (Saved to timing_cfg NVS immediately)\r\n", key, v);
        return;
      }
      case PARAM_FRAMING_CH1:
        applyFraming(g_config.uart_data_bits, g_config.uart_parity, g_config.uart_stop_bits, value, sock, key);
        return;
      case PARAM_FRAMING_CH2:
        applyFraming(g_config.ch2_data_bits, g_config.ch2_parity, g_config.ch2_stop_bits, value, sock, key);
        return;
      case PARAM_FRAMING_CH3:
        applyFraming(g_config.ch3_data_bits, g_config.ch3_parity, g_config.ch3_stop_bits, value, sock, key);
        return;
      case PARAM_FRAMING_CH4:
        applyFraming(g_config.doorphone_data_bits, g_config.doorphone_parity, g_config.doorphone_stop_bits, value, sock, key);
        return;
      case PARAM_STRING: {
        if (strlen(value) <= p.maxVal) {
          strncpy(p.ptr.str, value, p.maxVal);
          p.ptr.str[p.maxVal] = '\0';
          g_config_dirty.store(true, std::memory_order_relaxed);
          sendTelnetMsgf(sock, "[OK] Set %s = '%s' (RAM only. Use 'save' to commit to NVS)\r\n", key, value);
        } else {
          sendTelnetMsgf(sock, "[ERROR] String exceeds maximum length of %lu characters.\r\n", (unsigned long)p.maxVal);
        }
        return;
      }
      case PARAM_PASS_HASH: {
        char hash_hex[68];
        System_Sha256ToHex(value, hash_hex);
        strncpy(g_config.telnet_pass_hash, hash_hex, sizeof(g_config.telnet_pass_hash) - 1);
        g_config.telnet_pass_hash[sizeof(g_config.telnet_pass_hash) - 1] = '\0';
        g_config_dirty.store(true, std::memory_order_relaxed);
        sendTelnetMsg(sock, "[OK] Telnet password updated & SHA-256 hashed. Use 'save' to commit to NVS.\r\n");
        return;
      }
      }
    }
  }

  sendTelnetMsgf(sock, "[ERROR] Unknown parameter '%s'. Type 'config ?' to list all valid parameters.\r\n", key);
}

void cmdConfig(EmbeddedCli *cli, char *args, void *context) {
  int sock = getSock(context);
  int argc = embeddedCliGetTokenCount(args);

  if (argc == 0) {
    printConfig(sock);
    return;
  }

  const char *sub = embeddedCliGetToken(args, 1);
  if (strcmp(sub, "?") == 0 || strcasecmp(sub, "help") == 0) {
    printConfigHelp(sock);
    return;
  }

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
    sendTelnetMsg(sock, "[OK] Runtime configuration reset to system factory defaults. (RAM only. Use 'save' to commit)\r\n");
    return;
  }

  sendTelnetMsg(sock, "Usage: config [set <key> <value> | reset | ? | help]\r\n");
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

    if (argc >= 3) {
      const char *tok3 = embeddedCliGetToken(args, 3);
      if (strchr(tok3, '.') != nullptr) {
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

  out.append(Fmt::DIV80EQ);
  out.append("\r\n");
  sendTelnetMsgLen(sock, out.buf, out.offset);
}

} // namespace ConfigCli
