#pragma once

#include "CliCommands.h"
#include "MgmtRpc.h"
#include "TelnetCli.h"
#include "WallpadParser.h"
#include "ControlTemplate.h"
#include "ProfileMatcher.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

static inline TelnetManager::TelnetSession *getSession(void *ctx) noexcept {
  return static_cast<TelnetManager::TelnetSession *>(ctx);
}

static inline int getSock(void *ctx) noexcept {
  auto *s = getSession(ctx);
  return s ? s->sock : -1;
}

extern char g_cli_scratch_buf[5120];
