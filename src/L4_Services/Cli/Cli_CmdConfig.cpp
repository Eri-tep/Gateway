#include "L4_Services/CLI_Commands.h"
#include "L3_Protocol/Public/Protocol_Facade.h"
#include "L0_Foundation/System_Platform.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include <WiFi.h>

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

static RuntimeConfig s_staged_config;
static RuntimeTimingConfig s_staged_timing;
static bool s_staged_initialized = false;
static bool s_has_staged_changes = false;

static void ensureStagingInitialized() {
  if (!s_staged_initialized) {
    s_staged_config = Config_Get();
    s_staged_timing = TimingConfig_Get();
    s_staged_initialized = true;
    s_has_staged_changes = false;
  }
}

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
    {"ch1_baud",
     PARAM_UINT32,
     {.u32 = &s_staged_config.uart_baud_rate},
     1200,
     115200,
     "CH1 Device Master Baudrate (bps)"},
    {"ch2_baud",
     PARAM_UINT32,
     {.u32 = &s_staged_config.ch2_baud_rate},
     1200,
     115200,
     "CH2 Main Wallpad Baudrate (bps)"},
    {"ch3_baud",
     PARAM_UINT32,
     {.u32 = &s_staged_config.ch3_baud_rate},
     1200,
     115200,
     "CH3 Sub Wallpad Baudrate (bps)"},
    {"ch4_baud",
     PARAM_UINT32,
     {.u32 = &s_staged_config.doorphone_baud_rate},
     1200,
     115200,
     "CH4 Doorphone Baudrate (bps)"},

    // Channel Framings (8N1, 8E1, 8O1, 8N2)
    {"ch1_framing",
     PARAM_FRAMING_CH1,
     {.u8 = nullptr},
     0,
     0,
     "CH1 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch2_framing",
     PARAM_FRAMING_CH2,
     {.u8 = nullptr},
     0,
     0,
     "CH2 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch3_framing",
     PARAM_FRAMING_CH3,
     {.u8 = nullptr},
     0,
     0,
     "CH3 Framing (8N1, 8E1, 8O1, 8N2)"},
    {"ch4_framing",
     PARAM_FRAMING_CH4,
     {.u8 = nullptr},
     0,
     0,
     "CH4 Framing (8N1, 8E1, 8O1, 8N2)"},

    // Channel Delays
    {"ch1_poll",
     PARAM_TIMING_CH1,
     {.u16 = &s_staged_timing.ch1_poll_interval_ms},
     200,
     5000,
     "CH1 Master Polling Interval (ms)"},
    {"ch2_ack",
     PARAM_TIMING_CH2,
     {.u16 = &s_staged_timing.ch2_cache_delay_ms},
     5,
     300,
     "CH2 Main Wallpad Virtual ACK (ms)"},
    {"ch3_ack",
     PARAM_TIMING_CH3,
     {.u16 = &s_staged_timing.ch3_cache_delay_ms},
     20,
     1000,
     "CH3 Sub Wallpad Virtual ACK (ms)"},

    // Wallpad Profile (Dynamic immediate action)
    {"profile",
     PARAM_UCHAR,
     {.u8 = nullptr},
     0,
     3,
     "Wallpad Profile Slot (0=Auto, 1=Custom1, etc)"},

    // Wi-Fi & Network
    {"wifi_ssid",
     PARAM_STRING,
     {.str = s_staged_config.wifi_ssid},
     0,
     sizeof(s_staged_config.wifi_ssid) - 1,
     "Station Wi-Fi SSID"},
    {"wifi_pass",
     PARAM_STRING,
     {.str = s_staged_config.wifi_password},
     0,
     sizeof(s_staged_config.wifi_password) - 1,
     "Station Wi-Fi Password"},
    {"ap_ssid",
     PARAM_STRING,
     {.str = s_staged_config.ap_ssid},
     0,
     sizeof(s_staged_config.ap_ssid) - 1,
     "SoftAP SSID"},
    {"ap_pass",
     PARAM_STRING,
     {.str = s_staged_config.ap_password},
     0,
     sizeof(s_staged_config.ap_password) - 1,
     "SoftAP Password"},
    {"wifi_timeout",
     PARAM_UINT16,
     {.u16 = &s_staged_config.wifi_connect_timeout_s},
     5,
     120,
     "Wi-Fi Connection Timeout (seconds)"},

    // Security
    {"telnet_pass",
     PARAM_PASS_HASH,
     {.str = s_staged_config.telnet_pass_hash},
     0,
     0,
     "Telnet Login Password"},
};
static constexpr size_t PARAM_COUNT = std::size(PARAM_TABLE);

void printConfig(int sock) {
  ensureStagingInitialized();
  withScratchBuf(sock, [](AppendBuf &out) {
    CliFmt::PrintBoxHeader(out, "RUNTIME GATEWAY CONFIGURATION (* = Staged Change)");
    static constexpr Column CFG_COLS[] = {
        {"Parameter Key", 25, Align::LEFT, Align::CENTER},
        {"Configured Value", 48, Align::LEFT, Align::CENTER},
    };
    TableRenderer table(out, CFG_COLS, 2);
    table.header(false);

    const auto &active = Config_Get();
    const auto &active_timing = TimingConfig_Get();
    const uint8_t active_profile = Config_GetWallpadProfile();

    auto rowf = [&](bool diff, const char *k, const char *fmt, ...) {
      FixedBuf<32> k_buf;
      if (diff) {
        k_buf.append("* ");
      }
      k_buf.append(k);
      FixedBuf<64> v;
      va_list va;
      va_start(va, fmt);
      v.appendFormatV(fmt, va);
      va_end(va);
      table.row({k_buf.c_str(), v.c_str()});
    };

    bool d_ssid = strcmp(s_staged_config.wifi_ssid, active.wifi_ssid) != 0;
    rowf(d_ssid, "wifi_ssid", "\"%s\"",
         s_staged_config.wifi_ssid[0] ? s_staged_config.wifi_ssid : "(Not Configured)");

    bool d_ap = strcmp(s_staged_config.ap_ssid, active.ap_ssid) != 0;
    rowf(d_ap, "ap_ssid", "\"%s\"",
         s_staged_config.ap_ssid[0] ? s_staged_config.ap_ssid : "(Disabled)");

    bool d_wtout = (s_staged_config.wifi_connect_timeout_s != active.wifi_connect_timeout_s);
    rowf(d_wtout, "wifi_timeout", "%u sec", s_staged_config.wifi_connect_timeout_s);

    bool d_pass = strcmp(s_staged_config.telnet_pass_hash, active.telnet_pass_hash) != 0;
    table.row({d_pass ? "* telnet_pass" : "telnet_pass",
               s_staged_config.telnet_pass_hash[0]
                   ? "Configured (SHA-256)"
                   : "Default (None)"});

    table.row({"wallpad_profile",
               (active_profile == 1)   ? "1 (Custom Slot 1)"
               : (active_profile == 2) ? "2 (Custom Slot 2)"
               : (active_profile == 3)
                   ? "3 (Custom Slot 3)"
                   : "0 (Auto-Discovered & Learned)"});

    bool d_b1 = (s_staged_config.uart_baud_rate != active.uart_baud_rate) ||
                (s_staged_config.uart_data_bits != active.uart_data_bits) ||
                (s_staged_config.uart_parity != active.uart_parity) ||
                (s_staged_config.uart_stop_bits != active.uart_stop_bits);
    rowf(d_b1, "uart_baud_rate (CH1)", "%u bps (%s)",
         static_cast<unsigned>(s_staged_config.uart_baud_rate),
         formatFramingStr(s_staged_config.uart_data_bits, s_staged_config.uart_parity,
                          s_staged_config.uart_stop_bits));

    bool d_b2 = (s_staged_config.ch2_baud_rate != active.ch2_baud_rate) ||
                (s_staged_config.ch2_data_bits != active.ch2_data_bits) ||
                (s_staged_config.ch2_parity != active.ch2_parity) ||
                (s_staged_config.ch2_stop_bits != active.ch2_stop_bits);
    rowf(d_b2, "ch2_baud_rate (CH2)", "%u bps (%s)",
         static_cast<unsigned>(s_staged_config.ch2_baud_rate),
         formatFramingStr(s_staged_config.ch2_data_bits, s_staged_config.ch2_parity,
                          s_staged_config.ch2_stop_bits));

    bool d_b3 = (s_staged_config.ch3_baud_rate != active.ch3_baud_rate) ||
                (s_staged_config.ch3_data_bits != active.ch3_data_bits) ||
                (s_staged_config.ch3_parity != active.ch3_parity) ||
                (s_staged_config.ch3_stop_bits != active.ch3_stop_bits);
    rowf(d_b3, "ch3_baud_rate (CH3)", "%u bps (%s)",
         static_cast<unsigned>(s_staged_config.ch3_baud_rate),
         formatFramingStr(s_staged_config.ch3_data_bits, s_staged_config.ch3_parity,
                          s_staged_config.ch3_stop_bits));

    bool d_b4 = (s_staged_config.doorphone_baud_rate != active.doorphone_baud_rate) ||
                (s_staged_config.doorphone_data_bits != active.doorphone_data_bits) ||
                (s_staged_config.doorphone_parity != active.doorphone_parity) ||
                (s_staged_config.doorphone_stop_bits != active.doorphone_stop_bits);
    rowf(d_b4, "doorphone_baud_rate (CH4)", "%u bps (%s)",
         static_cast<unsigned>(s_staged_config.doorphone_baud_rate),
         formatFramingStr(s_staged_config.doorphone_data_bits,
                          s_staged_config.doorphone_parity,
                          s_staged_config.doorphone_stop_bits));

    bool d_p1 = (s_staged_timing.ch1_poll_interval_ms != active_timing.ch1_poll_interval_ms);
    rowf(d_p1, "ch1_poll_interval", "%u ms", s_staged_timing.ch1_poll_interval_ms);
    bool d_p2 = (s_staged_timing.ch2_cache_delay_ms != active_timing.ch2_cache_delay_ms);
    rowf(d_p2, "ch2_cache_delay", "%u ms", s_staged_timing.ch2_cache_delay_ms);
    bool d_p3 = (s_staged_timing.ch3_cache_delay_ms != active_timing.ch3_cache_delay_ms);
    rowf(d_p3, "ch3_cache_delay", "%u ms", s_staged_timing.ch3_cache_delay_ms);
    table.end('-');
    CliFmt::PrintBoxFooter(
        out, "Use 'config set <key> <val>', 'config discard', 'save' to commit");
  });
}

void printConfigHelp(int sock) {
  withScratchBuf(sock, [](AppendBuf &out) {
    CliFmt::PrintBoxHeader(out, "CONFIGURABLE PARAMETERS GUIDE");
    static constexpr Column CONFIG_HELP_COLS[] = {
        {"Parameter Key", 16, Align::LEFT, Align::LEFT},
        {"Allowed Range / Type", 20, Align::LEFT, Align::LEFT},
        {"Description", 34, Align::LEFT, Align::LEFT},
    };
    TableRenderer table(out, CONFIG_HELP_COLS, 3);
    table.header(false);

    FixedBuf<24> range_buf;
    for (size_t i = 0; i < PARAM_COUNT; ++i) {
      const auto &p = PARAM_TABLE[i];
      auto fmt_range = [&]() {
        if (p.type <= PARAM_UCHAR ||
            (p.type >= PARAM_TIMING_CH1 && p.type <= PARAM_TIMING_CH3)) {
          range_buf.appendFormat("%lu ~ %lu",
                                 (unsigned long)p.minVal, (unsigned long)p.maxVal);
          return;
        }
        if (p.type >= PARAM_FRAMING_CH1 && p.type <= PARAM_FRAMING_CH4) {
          range_buf.append("8N1,8E1,8O1,8N2");
          return;
        }
        range_buf.append(
            (p.type == PARAM_PASS_HASH) ? "string (raw)" : "string");
      };
      fmt_range();
      table.row({p.name, range_buf.c_str(), p.desc});
    }

    table.end('-');
    out.append("|  config set <key> <value>   : Modify parameter (RAM only)    "
               "                |\r\n");
    out.append("|  save                       : Commit modified parameters to "
               "NVS flash        |\r\n");
    out.append("|  config reset               : Restore all configuration to "
               "factory defaults  |\r\n");
    CliFmt::PrintBoxFooter(out,
                           "Tip: Use 'save' to commit changes to NVS flash");
  });
}

template <typename T>
static bool applyUintParam(T *dest, const char *value, unsigned long min_val,
                           unsigned long max_val, int sock, const char *key) {
  char *endp = nullptr;
  unsigned long v = strtoul(value, &endp, 10);
  if (!endp || *endp != '\0' || v < min_val || v > max_val) {
    sendTelnetMsgf(
        sock, "[ERROR] Invalid value '%s' for '%s' (Allowed: %lu ~ %lu)\r\n",
        value, key, min_val, max_val);
    return false;
  }
  *dest = static_cast<T>(v);
  s_has_staged_changes = true;
  sendTelnetMsgf(
      sock,
      "[OK] Staged %s = %lu (Run 'save' and 'system restart' to apply)\r\n",
      key, v);
  return true;
}

static bool applyFraming(uint8_t &dbits, uint8_t &parity, uint8_t &sbits,
                         const char *value, int sock, const char *key) {
  uint8_t d, p, s;
  if (!parseFramingStr(value, d, p, s)) {
    sendTelnetMsgf(
        sock,
        "[ERROR] Invalid framing '%s'. Choose from: 8N1, 8E1, 8O1, 8N2\r\n",
        value);
    return false;
  }
  dbits = d;
  parity = p;
  sbits = s;
  s_has_staged_changes = true;
  sendTelnetMsgf(
      sock,
      "[OK] Staged %s = '%s' (Run 'save' and 'system restart' to apply)\r\n",
      key, value);
  return true;
}

void setConfig(int sock, const char *key, const char *value) {
  if (!key || !value) {
    sendTelnetMsg(sock,
                  "[ERROR] Missing argument: config set <key> <value>\r\n");
    return;
  }
  ensureStagingInitialized();

  for (size_t i = 0; i < PARAM_COUNT; ++i) {
    const auto &p = PARAM_TABLE[i];
    if (strcasecmp(p.name, key) == 0) {
      switch (p.type) {
      case PARAM_UINT32:
        return (void)applyUintParam(p.ptr.u32, value, p.minVal, p.maxVal, sock,
                                    key);
      case PARAM_UINT16:
        return (void)applyUintParam(p.ptr.u16, value, p.minVal, p.maxVal, sock,
                                    key);
      case PARAM_UCHAR: {
        char *endp = nullptr;
        unsigned long v = strtoul(value, &endp, 10);
        if (!endp || *endp != '\0' || v > 3) {
          sendTelnetMsg(sock, "[ERROR] Profile slot must be 0 ~ 3.\r\n");
          return;
        }
        Config_SetWallpadProfile(static_cast<uint8_t>(v));
        Config_SaveWallpadProfile();
        sendTelnetMsgf(
            sock,
            "[OK] Active wallpad profile set to %lu & saved immediately.\r\n",
            v);
        return;
      }
      case PARAM_TIMING_CH1:
      case PARAM_TIMING_CH2:
      case PARAM_TIMING_CH3:
        return (void)applyUintParam(p.ptr.u16, value, p.minVal, p.maxVal, sock,
                                    key);
      case PARAM_FRAMING_CH1:
        return (void)applyFraming(s_staged_config.uart_data_bits,
                                  s_staged_config.uart_parity,
                                  s_staged_config.uart_stop_bits, value, sock,
                                  key);
      case PARAM_FRAMING_CH2:
        return (void)applyFraming(s_staged_config.ch2_data_bits,
                                  s_staged_config.ch2_parity,
                                  s_staged_config.ch2_stop_bits, value, sock,
                                  key);
      case PARAM_FRAMING_CH3:
        return (void)applyFraming(s_staged_config.ch3_data_bits,
                                  s_staged_config.ch3_parity,
                                  s_staged_config.ch3_stop_bits, value, sock,
                                  key);
      case PARAM_FRAMING_CH4:
        return (void)applyFraming(s_staged_config.doorphone_data_bits,
                                  s_staged_config.doorphone_parity,
                                  s_staged_config.doorphone_stop_bits, value,
                                  sock, key);
      case PARAM_STRING: {
        const size_t in_len = strlen(value);
        if (in_len <= p.maxVal) {
          memcpy(p.ptr.str, value, in_len);
          p.ptr.str[in_len] = '\0';
          s_has_staged_changes = true;
          sendTelnetMsgf(
              sock,
              "[OK] Staged %s = '%s' (Run 'save' and 'system restart' to apply)\r\n",
              key, value);
        } else {
          sendTelnetMsgf(
              sock,
              "[ERROR] String exceeds maximum length of %lu characters.\r\n",
              (unsigned long)p.maxVal);
        }
        return;
      }
      case PARAM_PASS_HASH: {
        char hash_hex[68];
        System_Sha256ToHex(value, hash_hex);
        strncpy(s_staged_config.telnet_pass_hash, hash_hex,
                sizeof(s_staged_config.telnet_pass_hash) - 1);
        s_staged_config.telnet_pass_hash[sizeof(s_staged_config.telnet_pass_hash) - 1] =
            '\0';
        s_has_staged_changes = true;
        sendTelnetMsg(sock, "[OK] Staged telnet password (SHA-256 hashed). "
                            "Run 'save' and 'system restart' to apply.\r\n");
        return;
      }
      default:
        break;
      }
    }
  }

  sendTelnetMsgf(sock,
                 "[ERROR] Unknown parameter '%s'. Type 'config ?' to list all "
                 "valid parameters.\r\n",
                 key);
}

void cmdConfig(CliContext &ctx) {
  int sock = ctx.sock;
  int argc = ctx.args.count();
  if (argc == 0) {
    printConfig(sock);
    return;
  }
  ensureStagingInitialized();

  static const CliFmt::SubCmdDef kConfigDefs[] = {
      {"list", "list", "Display runtime configuration table",
       [](int s, int, const Args &) { printConfig(s); }},
      {"set", "set <key> <value>", "Stage configuration parameter in buffer",
       [](int s, int ac, const Args &args) {
         if (ac >= 3)
           setConfig(s, args.get(2), args.get(3));
         else
           sendTelnetMsg(
               s, "[ERROR] Missing argument: config set <key> <value>\r\n");
       }},
      {"discard", "discard", "Discard uncommitted staged changes",
       [](int s, int, const Args &) {
         s_staged_config = Config_Get();
         s_staged_timing = TimingConfig_Get();
         s_has_staged_changes = false;
         sendTelnetMsg(
             s,
             "[OK] Staged changes discarded. Reverted to active config.\r\n");
       }},
      {"reset", "reset", "Reset staged configuration to factory defaults",
       [](int s, int, const Args &) {
         s_staged_config = RuntimeConfig{};
         s_staged_timing = RuntimeTimingConfig{};
         s_has_staged_changes = true;
         sendTelnetMsg(
             s, "[OK] Staged configuration reset to factory defaults. "
                "Run 'save' to commit to NVS.\r\n");
       }},
  };

  const char *sub = ctx.args.get(1);
  if (CliFmt::DispatchSubCmd(sub, sock, argc, ctx.args, kConfigDefs))
    return;

  printConfigHelp(sock);
}

void cmdSave(CliContext &ctx) {
  int sock = ctx.sock;
  ensureStagingInitialized();
  if (Config_SaveStaged(s_staged_config, s_staged_timing)) {
    s_has_staged_changes = false;
    sendTelnetMsg(
        sock,
        "[OK] Configuration successfully committed and saved to NVS flash!\r\n"
        "[NOTICE] Run 'system restart' to apply modified parameters.\r\n");
  } else {
    sendTelnetMsg(
        sock,
        "[ERROR] Failed to validate or commit staged configuration to NVS.\r\n");
  }
}

static bool ew11ParseSlot(int sock, const char *arg, int &slot,
                          const char *cmd) {
  if (!arg) {
    sendTelnetMsgf(sock, "[ERROR] Missing slot: ew11 %s <slot:0-4>\r\n", cmd);
    return false;
  }
  if (!CliFmt::ParseInt(arg, slot, 0, Config::TCP::MAX_EW11_SLOTS - 1)) {
    sendTelnetMsgf(sock, "[ERROR] Slot index must be 0 to %d\r\n",
                   Config::TCP::MAX_EW11_SLOTS - 1);
    return false;
  }
  return true;
}

static void ew11SetEnable(int sock, int slot, bool enabled) {
  ProtocolDiag_SetBridgeSlotEnabled(static_cast<uint8_t>(slot), enabled);
  sendTelnetMsgf(sock, "[OK] EW11 Slot #%d %s and saved to NVS flash.\r\n",
                 slot, enabled ? "ENABLED" : "DISABLED");
}

static void ew11PrintStatus(int sock) {
  withScratchBuf(sock, [](AppendBuf &out) {
    CliFmt::PrintBoxHeader(out, "CH5 EW11 TCP CLIENT SOCKET STATUS");
    static constexpr Column EW11_COLS[] = {
        {"Slot", 4, Align::CENTER, Align::CENTER},
        {"Name", 10, Align::LEFT, Align::CENTER},
        {"Port", 4, Align::CENTER, Align::CENTER},
        {"Client IP", 15, Align::CENTER, Align::CENTER},
        {"Status", 11, Align::CENTER, Align::CENTER},
        {"Packets", 17, Align::CENTER, Align::CENTER},
    };
    TableRenderer table_sock(out, EW11_COLS, 6);
    table_sock.header(false);

    {
      FixedBuf<8> s_buf, p_buf;
      FixedBuf<24> pkt_buf;
      for (int s = 0; s < Config::TCP::MAX_EW11_SLOTS; s++) {
        HubClientSlotSnapshot slot;
        System_GetBridgeSlotSnapshot(static_cast<uint8_t>(s), slot);
        const char *status_str = !slot.enabled         ? "Disabled"
                                 : !slot.is_connected  ? "Listening"
                                 : (slot.rx_pkts == 0) ? "Idle"
                                                       : "Connected";
        const char *ip_str =
            slot.is_connected
                ? (slot.target_ip[0] ? slot.target_ip : "Connected")
                : (slot.target_ip[0] ? slot.target_ip : "-");
        s_buf.reset();
        p_buf.reset();
        pkt_buf.reset();
        s_buf.appendFormat("#%d", s);
        p_buf.appendFormat("%u", slot.target_port);
        pkt_buf.appendFormat("%8u / %-8u",
                             static_cast<unsigned>(slot.rx_pkts),
                             static_cast<unsigned>(slot.tx_pkts));
        table_sock.row(
            {s_buf.c_str(), slot.name, p_buf.c_str(), ip_str, status_str, pkt_buf.c_str()});
      }
    }
    table_sock.end('-');
    CliFmt::PrintBoxFooter(
        out, "Configured Max Slots: 5 | Bridge Target: CH1 & CH2/3");

    // ── FCU Modbus 실시간 상태 테이블 ──
    CliFmt::PrintBoxHeader(out, "CH5 FCU MODBUS DEVICE STATUS");
    static constexpr Column FCU_COLS[] = {
        {"Slot", 4, Align::CENTER, Align::CENTER},
        {"Name", 6, Align::LEFT, Align::CENTER},
        {"Port", 4, Align::CENTER, Align::CENTER},
        {"Client IP", 14, Align::CENTER, Align::CENTER},
        {"Pwr", 3, Align::CENTER, Align::CENTER},
        {"Mode", 4, Align::CENTER, Align::CENTER},
        {"Fan", 4, Align::CENTER, Align::CENTER},
        {"Swng", 4, Align::CENTER, Align::CENTER},
        {"Tgt", 3, Align::CENTER, Align::CENTER},
        {"Room", 3, Align::CENTER, Align::CENTER},
    };
    TableRenderer table_fcu(out, FCU_COLS, 10);
    table_fcu.header(false);

    {
      for (uint8_t s = 1; s < Config::TCP::MAX_EW11_SLOTS; ++s) {
        HubClientSlotSnapshot slot;
        System_GetBridgeSlotSnapshot(s, slot);
        FcuDeviceSnapshot snap;
        Device_GetFcuSnapshot(s, snap);

        static constexpr const char *kModes[] = {"-", "Cool", "Heat", "Fan"};
        static constexpr const char *kFans[] = {"OFF", "Low", "Mid", "High",
                                                "Auto"};
        uint16_t m_idx = snap.mode;
        uint16_t f_idx = snap.fan_speed;
        const char *pwr_str = snap.power ? "ON" : "OFF";
        const char *mode_str =
            (m_idx >= 1 && m_idx <= 3) ? kModes[m_idx] : "-";
        const char *fan_str = (f_idx <= 4) ? kFans[f_idx] : "-";
        const char *swng_str =
            (snap.swing == 2) ? "ON" : "OFF";

        char tgt_str[8] = "-", room_str[8] = "-";
        if (snap.is_online) {
          AppendBuf{tgt_str, sizeof(tgt_str)}.appendFormat("%uC", snap.target_temp);
          AppendBuf{room_str, sizeof(room_str)}.appendFormat("%uC", snap.room_temp);
        }
        const char *ip_str =
            (slot.is_connected && slot.target_ip[0]) ? slot.target_ip : "-";

        char s_buf[8], p_buf[8];
        AppendBuf{s_buf, sizeof(s_buf)}.appendFormat("#%u", s);
        AppendBuf{p_buf, sizeof(p_buf)}.appendFormat("%u", slot.target_port);

        table_fcu.row({s_buf, slot.name, p_buf, ip_str, pwr_str, mode_str,
                       fan_str, swng_str, tgt_str, room_str});
      }
    }
    table_fcu.end('-');
    out.append(CliFmt::BOX80_EQ);
    out.append("\r\n");
  });
}

void cmdEw11(CliContext &ctx) {
  int sock = ctx.sock;
  int argc = ctx.args.count();
  const char *sub = (argc > 0) ? ctx.args.get(1) : "list";

  // Unified table: help strings + handlers in one place.
  static const CliFmt::SubCmdDef kEw11Defs[] = {
      {"list", "list", "Show EW11 sockets & FCU runtime status",
       [](int s, int, const Args &) { ew11PrintStatus(s); }},
      {"status", "status", "Show EW11 sockets & FCU runtime status",
       [](int s, int, const Args &) { ew11PrintStatus(s); }},
      {"set", "set <slot> [port] [ip] [name] [en]",
       "Configure EW11 bridge socket settings",
       [](int sock, int argc, const Args &args) {
         int slot = -1;
         if (!ew11ParseSlot(sock, args.get(2), slot, "set"))
           return;
         HubClientSlotSnapshot slot_snap;
         System_GetBridgeSlotSnapshot(static_cast<uint8_t>(slot), slot_snap);
         uint16_t default_port = Config::TCP::EW11_SLOT_PORTS[slot];
         uint16_t port =
             slot_snap.target_port > 0 ? slot_snap.target_port : default_port;
         const char *ip_str = nullptr;
         const char *name_str = nullptr;
         bool enabled = slot_snap.enabled;
         for (int i = 3; i <= argc; ++i) {
           const char *tok = args.get(i);
           if (!tok || !*tok)
             continue;
           int v = 0;
           if (strchr(tok, '.') || strcmp(tok, "-") == 0 ||
               strcmp(tok, "none") == 0) {
             ip_str =
                 (strcmp(tok, "-") == 0 || strcmp(tok, "none") == 0) ? "" : tok;
             continue;
           }
           if (port == default_port &&
               CliFmt::ParseInt(tok, v, 1, 65535)) {
             port = static_cast<uint16_t>(v);
             continue;
           }
           if (!name_str && !isdigit(tok[0])) {
             name_str = tok;
             continue;
           }
           if (CliFmt::ParseInt(tok, v, 0, 1)) {
             enabled = (v != 0);
           }
         }
         if (ProtocolDiag_SetBridgeSlotConfig(static_cast<uint8_t>(slot), enabled, ip_str, port,
                                        name_str)) {
           System_GetBridgeSlotSnapshot(static_cast<uint8_t>(slot), slot_snap);
           sendTelnetMsgf(
               sock,
               "[OK] EW11 Slot #%d configured (Name: %s, Listen Port: %u, "
               "Allowed IP: %s, Enabled: %s) and saved to NVS!\r\n",
               slot, slot_snap.name, slot_snap.target_port,
               slot_snap.target_ip[0] ? slot_snap.target_ip : "Any",
               slot_snap.enabled ? "true" : "false");
         } else {
           sendTelnetMsg(sock, "[ERROR] Failed to configure EW11 slot.\r\n");
         }
       }},
      {"frame", "frame <slot> <stx> <etx> [len]",
       "Set custom framing delimiters for slot",
       [](int sock, int argc, const Args &args) {
         if (argc < 4) {
           sendTelnetMsg(sock, "[ERROR] Format: ew11 frame <slot:0-4> "
                               "<stx:hex> <etx:hex> [len:dec]\r\n");
           return;
         }
         int slot = -1;
         if (!ew11ParseSlot(sock, args.get(2), slot, "frame"))
           return;
         uint8_t stx = static_cast<uint8_t>(strtoul(args.get(3), nullptr, 16));
         uint8_t etx = static_cast<uint8_t>(strtoul(args.get(4), nullptr, 16));
         uint8_t len = 0;
         if (argc >= 5) {
           int parsed_len = 0;
           if (CliFmt::ParseInt(args.get(5), parsed_len, 0, 255))
             len = static_cast<uint8_t>(parsed_len);
         }
         ProtocolDiag_SetBridgeFramingLock(static_cast<uint8_t>(slot), stx, etx, len);
         sendTelnetMsgf(sock,
                        "[OK] EW11 Slot #%d framing permanently fixed to STX "
                        "0x%02X, ETX 0x%02X, Len %u.\r\n",
                        slot, stx, etx, len);
       }},
      {"reset", "reset <slot>", "Reset EW11 socket slot to defaults",
       [](int sock, int, const Args &args) {
         int slot = -1;
         if (!ew11ParseSlot(sock, args.get(2), slot, "reset"))
           return;
         ProtocolDiag_ResetBridgeFraming(static_cast<uint8_t>(slot));
         sendTelnetMsgf(sock,
                        "[OK] EW11 Slot #%d framing tracker reset to "
                        "autonomous auto-probing.\r\n",
                        slot);
       }},
      {"enable", "enable <slot>", "Enable specified EW11 socket slot",
       [](int sock, int, const Args &args) {
         int slot = -1;
         if (ew11ParseSlot(sock, args.get(2), slot, "enable"))
           ew11SetEnable(sock, slot, true);
       }},
      {"disable", "disable <slot>", "Disable specified EW11 socket slot",
       [](int sock, int, const Args &args) {
         int slot = -1;
         if (ew11ParseSlot(sock, args.get(2), slot, "disable"))
           ew11SetEnable(sock, slot, false);
       }},
  };

  if (CliFmt::IsHelp(sub)) {
    CliFmt::PrintSubCmdHelp(
        sock, "EW11 COMMAND REFERENCE", kEw11Defs,
        "Tip: Slot index 0 is Master Hub, 1-4 are FCU Bridges");
    return;
  }

  if (CliFmt::DispatchSubCmd(sub, sock, argc, ctx.args, kEw11Defs))
    return;

  CliFmt::PrintSubCmdHelp(
      sock, "EW11 COMMAND REFERENCE", kEw11Defs,
      "Tip: Slot index 0 is Master Hub, 1-4 are FCU Bridges");
}

void cmdRoutes(CliContext &ctx) {
  int sock = ctx.sock;
  int argc = ctx.args.count();

  static const CliFmt::SubCmdDef kRoutesDefs[] = {
      {"clear", "clear", "Clear dynamic device ingress routing table",
       [](int s, int, const Args &) {
         Protocol_ClearRoutes();
         sendTelnetMsg(s,
                       "[OK] Dynamic device ingress routing table cleared.\r\n");
       }},
  };

  if (argc == 1 &&
      CliFmt::DispatchSubCmd(ctx.args.get(1), sock, argc, ctx.args, kRoutesDefs)) {
    return;
  }

  static DeviceRouteSnapshot entries[64];
  size_t count = Protocol_GetRoutes(entries, 64);

  withScratchBuf(sock, [count](AppendBuf &out) {
    CliFmt::PrintBoxHeader(
        out, "DYNAMIC DEVICE INGRESS ROUTING TABLE (Zero Hardcode)");
    static constexpr Column ROUTES_COLS[] = {
        {"Target (DevID:Sub1:Sub2)", 24, Align::CENTER, Align::CENTER},
        {"Egress Destination", 33, Align::LEFT, Align::CENTER},
        {"Last Seen", 13, Align::CENTER, Align::CENTER},
    };
    TableRenderer table(out, ROUTES_COLS, 3);
    table.header(false);

    if (count == 0) {
      table.empty(
          "(No device routes learned yet. Waiting for bus/EW11 packets...)");
    } else {
      uint32_t now = millis();
      FixedBuf<24> tgt_str;
      FixedBuf<36> dst_str;
      for (size_t i = 0; i < count; i++) {
        const auto &e = entries[i];
        char el_str[20];
        tgt_str.reset();
        dst_str.reset();
        tgt_str.appendFormat("0x%02X:%02X:%02X", e.dev_id, e.sub1, e.sub2);
        if (e.channel_id == 5 && e.slot_idx >= 0) {
          dst_str.appendFormat("CH#5 Slot %d", e.slot_idx);
        } else {
          dst_str.appendFormat("CH#%u", e.channel_id);
        }
        Fmt::FormatElapsed(now, e.last_seen_ms, el_str,
                           sizeof(el_str));
        table.row({tgt_str.c_str(), dst_str.c_str(), el_str});
      }
    }
    table.end('-');
    CliFmt::PrintBoxFooter(out,
                           "Use 'routes clear' to reset dynamic route table");
  });
}

} // namespace ConfigCli

// ============================================================================
// From src/CLI/CliStatus.cpp
// ============================================================================
