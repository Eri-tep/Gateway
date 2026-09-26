#include "TelnetCli.h"
#include "esp_ota_ops.h"
#include <cstdio>
#include <cstring>
#include <cctype>

static bool Tcp_ConstantTimeStrcmp(const char *a, const char *b) {
  if (!a || !b)
    return false;
  size_t len_a = strlen(a), len_b = strlen(b);
  if (len_a != len_b)
    return false;
  volatile int result = 0;
  for (size_t i = 0; i < len_a; ++i)
    result |= a[i] ^ b[i];
  return result == 0;
}

namespace {
constexpr uint32_t AUTH_FAIL_PENALTY_MS = 5000;
} // namespace

TelnetManager::AuthResult TelnetManager::evaluateAuth(
    const char *clean_pw, const char *stored_hash,
    AuthBlockEntry *blk, uint32_t now_ms) {
  if (blk && blk->failedCount >= 3 &&
      !TimeUtils::isElapsed(blk->lastFailedMs, AUTH_FAIL_PENALTY_MS)) {
    return AuthResult::LOCKED_OUT;
  }

  char input_hash[68];
  System_Sha256ToHex(clean_pw, input_hash);

  bool auth_ok = false;
  if (strlen(stored_hash) > 0 &&
      Tcp_ConstantTimeStrcmp(input_hash, stored_hash)) {
    auth_ok = true;
  }

#ifdef DEFAULT_TELNET_PASS
  if (!auth_ok && strcasecmp(clean_pw, DEFAULT_TELNET_PASS) == 0) {
    auth_ok = true;
    {
      CriticalSectionLocker lock(&g_config_mux);
      strncpy(g_config.telnet_pass_hash, input_hash,
              sizeof(g_config.telnet_pass_hash) - 1);
      g_config.telnet_pass_hash[sizeof(g_config.telnet_pass_hash) - 1] = '\0';
    }
    Config_Save();
  }
#endif

  if (auth_ok) {
    if (blk) {
      blk->failedCount = 0;
      blk->lastFailedMs = 0;
    }
    return AuthResult::OK;
  }

  if (blk) {
    blk->failedCount++;
    blk->lastFailedMs = now_ms;
  }
  return AuthResult::WRONG_PASSWORD;
}

bool TelnetManager::handlePassword(TelnetSession *session,
                                   const char *password) {
  uint32_t now = millis();
  IPAddress clientIp = session->clientIp;

  AuthBlockEntry *blk = nullptr;
  for (int i = 0; i < 4; ++i) {
    if (_authBlocks[i].ip == clientIp) {
      blk = &_authBlocks[i];
      break;
    }
  }

  if (!blk) {
    for (int i = 0; i < 4; ++i) {
      if (_authBlocks[i].failedCount == 0 ||
          (now - _authBlocks[i].lastFailedMs > 60000)) {
        blk = &_authBlocks[i];
        blk->ip = clientIp;
        blk->failedCount = 0;
        blk->lastFailedMs = 0;
        break;
      }
    }
  }

  char clean_pw[64] = {0};
  size_t len = 0;
  for (size_t i = 0; password[i] != '\0' && len < sizeof(clean_pw) - 1; i++) {
    if (isprint((unsigned char)password[i]) && password[i] != ' ') {
      clean_pw[len++] = password[i];
    }
  }

  AuthResult res = evaluateAuth(clean_pw, g_config.telnet_pass_hash, blk, now);

  if (res == AuthResult::LOCKED_OUT) {
    sendTelnetMsg(
        session->sock,
        "\r\n[SECURITY] Too many failed attempts. Try again in 5 seconds.\r\n");
    return false;
  }

  if (res == AuthResult::OK) {
    session->sessionState = AUTHENTICATED;
    sendTelnetMsg(session->sock, "\r\nAuthentication successful.\r\n"
                                 "Welcome to Gateway Bridge Diagnostics!\r\n"
                                 "Type 'help' for available commands, "
                                 "or press [TAB] to auto-complete.\r\n\r\n");

    if (g_rollback_detected) {
      char warn_msg[384];
      const esp_partition_t *cur = esp_ota_get_running_partition();
      const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
      snprintf(warn_msg, sizeof(warn_msg),
               "================================================================================\r\n"
               " ⚠️  [SYSTEM AUTO-ROLLBACK NOTICE]\r\n"
               " ⚠️  Firmware automatically rolled back to stable partition '%s'!\r\n"
               " ⚠️  Failed partition '%s' crashed during boot and was invalidated.\r\n"
               "================================================================================\r\n\r\n",
               cur ? cur->label : "app0", other ? other->label : "app1");
      sendTelnetMsg(session->sock, warn_msg);
    }
    if (g_rescue_mode.load(std::memory_order_relaxed)) {
      sendTelnetMsg(session->sock,
               "================================================================================\r\n"
               " 🚨  [RESCUE SAFE MODE ACTIVE]\r\n"
               " 🚨  Connected via Emergency SoftAP (Sweet_Home_Rescue). RS-485 tasks bypassed.\r\n"
               " 🚨  Use 'ota status' or 'coredump' to diagnose and upload new firmware.\r\n"
               "================================================================================\r\n\r\n");
    }

    EmbeddedCliConfig *config = embeddedCliDefaultConfig();
    config->cliBuffer = nullptr;
    config->cliBufferSize = 0;
    config->rxBufferSize = 128;
    config->cmdBufferSize = 128;
    config->historyBufferSize = 256;
    config->maxBindingCount = 32;
    config->enableAutoComplete = true;

    session->cli.reset(embeddedCliNew(config));
    if (session->cli) {
      session->cli->appContext = session;
      session->cli->writeChar = writeCharToClient;
      bindCommands(session);
      embeddedCliProcess(session->cli.get());
      session->needsSend = true;
      return true;
    } else {
      session->sessionState = AWAITING_PASSWORD;
      sendTelnetMsg(session->sock,
                    "\r\n[ERROR] Out of memory to allocate CLI instance.\r\n");
      return false;
    }
  }

  sendTelnetMsg(session->sock, "\r\nInvalid password.\r\n");
  return false;
}
