#pragma once

// ============================================================================
// ConsoleCommands: Level 4 Interactive Console Commands & Diagnostics
// ============================================================================

#include "L4_Services/CLI_Service.h"

// ── CLI Subsystem ──

// ============================================================================
// CLI Subsystem Command Interfaces
// ============================================================================

namespace WifiCli {
void cmdWifi(CliContext &ctx);
} // namespace WifiCli

namespace WallpadCli {
void cmdWallpad(CliContext &ctx);
void cmdCtl(CliContext &ctx);
void cmdTrace(CliContext &ctx);
void cmdStop(CliContext &ctx);
void cmdDevs(CliContext &ctx);

void wallpadPrintStatus(AppendBuf &out);
void wallpadListProfiles(AppendBuf &out);
void wallpadSaveProfile(int sock, const char *name);
void wallpadDeleteProfile(int sock, const char *target);
void wallpadSetProfile(int sock, const char *key);

void devsPrintSummary(AppendBuf &out, uint32_t now);
void devsPrintTier1Targets(AppendBuf &out, uint32_t now);
void devsPrintTier2Cache(AppendBuf &out, uint32_t now);

void wallpadPrintControlTable(AppendBuf &out);
void wallpadPrintControlDetail(AppendBuf &out, uint8_t dev_id);
} // namespace WallpadCli

namespace SystemCli {
void cmdStats(CliContext &ctx);
void cmdReboot(CliContext &ctx);
void cmdLogView(CliContext &ctx);
void cmdCoreDump(CliContext &ctx);
void cmdOta(CliContext &ctx);
void cmdHelp(CliContext &ctx);

void printStats(int sock);
void printSystemOverview(AppendBuf &out);
void otaPrintStatus(AppendBuf &out);
void otaTriggerRollback(int sock);
void otaValidate(int sock);
} // namespace SystemCli

namespace ConfigCli {
void cmdConfig(CliContext &ctx);
void cmdSave(CliContext &ctx);
void cmdEw11(CliContext &ctx);
void cmdRoutes(CliContext &ctx);
void printConfig(int sock);
void setConfig(int sock, const char *key, const char *value);
} // namespace ConfigCli

// ============================================================================
// Unified Command Table Definition
// ============================================================================

struct CommandDef {
  const char *name;
  const char *help;
  void (*handler)(CliContext &ctx);
};

extern const CommandDef kConsoleCmds[];
extern const size_t kConsoleCmdsCount;
