#pragma once

#include "Common.h"
#include <embedded_cli.h>

namespace WifiCli {
void cmdWifi(EmbeddedCli *cli, char *args, void *context);
} // namespace WifiCli

namespace WallpadCli {
void cmdWallpad(EmbeddedCli *cli, char *args, void *context);
void cmdCtl(EmbeddedCli *cli, char *args, void *context);
void cmdTrace(EmbeddedCli *cli, char *args, void *context);
void cmdStop(EmbeddedCli *cli, char *args, void *context);
void cmdDevs(EmbeddedCli *cli, char *args, void *context);

void wallpadPrintStatus(AppendBuf &out);
void wallpadListProfiles(AppendBuf &out);
void wallpadSaveProfile(int sock, const char *name);
void wallpadDeleteProfile(int sock, const char *target);
void wallpadSetProfile(int sock, const char *key);

void devsPrintTier1Targets(AppendBuf &out, uint32_t now);
void devsPrintTier2Cache(AppendBuf &out, uint32_t now);

void wallpadPrintControlTable(AppendBuf &out);
void wallpadPrintControlDetail(AppendBuf &out, uint8_t dev_id);
void wallpadControlLearn(int sock, uint8_t dev_id);
void wallpadPrintControlLearnStatus(AppendBuf &out);
void wallpadControlAbort(int sock);
void wallpadControlReset(int sock, uint8_t dev_id);
} // namespace WallpadCli

namespace SystemCli {
void cmdStats(EmbeddedCli *cli, char *args, void *context);
void cmdReboot(EmbeddedCli *cli, char *args, void *context);
void cmdLogView(EmbeddedCli *cli, char *args, void *context);
void cmdCoreDump(EmbeddedCli *cli, char *args, void *context);
void cmdOta(EmbeddedCli *cli, char *args, void *context);
void cmdHelp(EmbeddedCli *cli, char *args, void *context);

void printStats(int sock);
void printSystemOverview(AppendBuf &out);
void otaPrintStatus(AppendBuf &out);
void otaTriggerRollback(int sock);
void otaValidate(int sock);
} // namespace SystemCli

namespace ConfigCli {
void cmdConfig(EmbeddedCli *cli, char *args, void *context);
void cmdSave(EmbeddedCli *cli, char *args, void *context);
void printConfig(int sock);
void setConfig(void *session_context, const char *key, const char *value);
} // namespace ConfigCli
