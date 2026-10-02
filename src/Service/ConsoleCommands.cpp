#include "Service/ConsoleCommands.h"
#include "Service/Console/ConsoleFmt.h"
#include "Service/ConsoleCli.h"

// ============================================================================
// Shared CLI 5KB Scratch Buffer Implementation
// ============================================================================

static char g_cli_scratch_buf[5120];

char *Cli_GetScratchBuffer() {
  return g_cli_scratch_buf;
}

size_t Cli_GetScratchBufferSize() {
  return sizeof(g_cli_scratch_buf);
}

void withScratchBufInternal(int sock, std::function<void(AppendBuf &)> fn) {
  g_cli_scratch_buf[0] = '\0';
  AppendBuf out{g_cli_scratch_buf, sizeof(g_cli_scratch_buf)};
  fn(out);
  if (sock >= 0 && out.offset > 0) {
    sendTelnetMsgLen(sock, out.buf, out.offset);
  }
}

// ============================================================================
// Top-Level Unified Command Table Dispatch Definition
// ============================================================================

const CommandDef kConsoleCmds[] = {
    {"stats", "Show real-time HW metrics & traffic stats [clear]",
     SystemCli::cmdStats},
    {"devs", "Show device registry & cache [1|2|all|clear]",
     WallpadCli::cmdDevs},
    {"wifi", "Manage WiFi connection [status|scan|connect|disconnect]",
     WifiCli::cmdWifi},
    {"trace", "Packet monitoring [on|off|ctl|ack|pol|rmt|drp|ch|devid]",
     WallpadCli::cmdTrace},
    {"wallpad",
     "Wallpad protocol & auto-probing "
     "[status|list|set|save|delete|auto|reset|simulate]",
     WallpadCli::cmdWallpad},
    {"ctl", "Device control blueprints [table|<dev_id>|name|class|reset]",
     WallpadCli::cmdCtl},
    {"config", "View or modify runtime configuration [set|reset]",
     ConfigCli::cmdConfig},
    {"save", "Save current runtime configuration to NVS flash",
     ConfigCli::cmdSave},
    {"ew11", "CH5 EW11 hub sockets & FCU [list|set|frame|reset|enable|disable]",
     ConfigCli::cmdEw11},
    {"routes", "Show dynamic device ingress routing table [clear]",
     ConfigCli::cmdRoutes},
    {"logview",
     "Persistent reboot history & crash logs [list|<1-20>|last|clear]",
     SystemCli::cmdLogView},
    {"coredump", "Show crash core dump summary or erase partition [clear]",
     SystemCli::cmdCoreDump},
    {"ota", "Dual-partition OTA & rollback [status|rollback|validate|cloud]",
     SystemCli::cmdOta},
    {"reboot", "Perform hardware system reboot with safe shutdown",
     SystemCli::cmdReboot},
    {"q", "Stop active packet tracing (shortcut for 'trace off')",
     WallpadCli::cmdStop},
    {"exit", "Disconnect current Telnet CLI session", TelnetManager::cmdExit},
    {"help", "Display comprehensive command reference and usage examples",
     SystemCli::cmdHelp}};

const size_t kConsoleCmdsCount = sizeof(kConsoleCmds) / sizeof(kConsoleCmds[0]);
