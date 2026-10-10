#pragma once

// ============================================================================
// CLI_Commands: Level 4 Interactive Console Commands & 80-Col Formatting (C++23)
// ============================================================================

#include "L4_Services/CLI_Service.h"
#include <climits>
#include <cstdlib>
#include <cstring>
#include <functional>

// ── Shared Scratch Buffer Interface ─────────────────────────────────────────
char *Cli_GetScratchBuffer();
size_t Cli_GetScratchBufferSize();
void withScratchBufInternal(int sock, std::function<void(AppendBuf &)> fn);

template <typename F> inline void withScratchBuf(int sock, F &&fn) {
  withScratchBufInternal(sock, std::forward<F>(fn));
}

// ── CLI 80-Column Unified Formatting & Subcommand Helpers ───────────────────
namespace CliFmt {
constexpr char BOX80_EQ[] = "+================================================="
                            "=============================+\r\n";
constexpr char BOX80_DASH[] = "+-----------------------------------------------"
                              "-------------------------------+\r\n";

inline bool ParseInt(const char *s, int &out_val, int min_v = INT_MIN,
                     int max_v = INT_MAX) noexcept {
  if (!s || !*s)
    return false;
  char *endp = nullptr;
  long v = strtol(s, &endp, 10);
  if (endp == s || *endp != '\0' || v < min_v || v > max_v)
    return false;
  out_val = static_cast<int>(v);
  return true;
}

inline void PrintBoxHeader(AppendBuf &out, const char *title) {
  out.append("\r\n");
  out.append(BOX80_EQ);
  int tlen = title ? static_cast<int>(strlen(title)) : 0;
  if (tlen > 78)
    tlen = 78;
  int pad_l = (78 - tlen) / 2;
  int pad_r = 78 - tlen - pad_l;
  out.appendFormat("|%*s%.*s%*s|\r\n", pad_l, "", tlen, title ? title : "",
                   pad_r, "");
  out.append(BOX80_EQ);
}

__attribute__((format(printf, 2, 3))) inline void
PrintBoxHeaderf(AppendBuf &out, const char *fmt, ...) {
  FixedBuf<80> fb;
  va_list args;
  va_start(args, fmt);
  fb.appendFormatV(fmt, args);
  va_end(args);
  PrintBoxHeader(out, fb.c_str());
}

inline void PrintBoxSubtitle(AppendBuf &out, const char *subtitle) {
  int slen = subtitle ? static_cast<int>(strlen(subtitle)) : 0;
  if (slen > 78)
    slen = 78;
  int pad_l = (78 - slen) / 2;
  int pad_r = 78 - slen - pad_l;
  out.appendFormat("|%*s%.*s%*s|\r\n", pad_l, "", slen,
                   subtitle ? subtitle : "", pad_r, "");
}

__attribute__((format(printf, 2, 3))) inline void
PrintBoxSubtitlef(AppendBuf &out, const char *fmt, ...) {
  FixedBuf<80> fb;
  va_list args;
  va_start(args, fmt);
  fb.appendFormatV(fmt, args);
  va_end(args);
  PrintBoxSubtitle(out, fb.c_str());
}

inline void PrintBoxFooter(AppendBuf &out, const char *tip) {
  int tlen = tip ? static_cast<int>(strlen(tip)) : 0;
  if (tlen > 78)
    tlen = 78;
  int pad_l = (78 - tlen) / 2;
  int pad_r = 78 - tlen - pad_l;
  out.appendFormat("|%*s%.*s%*s|\r\n", pad_l, "", tlen, tip ? tip : "", pad_r,
                   "");
  out.append(BOX80_EQ);
  out.append("\r\n");
}

inline bool IsHelp(const char *s) {
  return s && (s[0] == '?' || strcasecmp(s, "help") == 0);
}

struct SubCmdDef {
  const char *name;
  const char *syntax;
  const char *desc;
  void (*handler)(int sock, int argc, const Args &args);
};

inline void PrintSubCmdHelp(int sock, const char *title, const SubCmdDef *defs,
                            size_t count, const char *tip = nullptr) {
  withScratchBuf(sock, [title, defs, count, tip](AppendBuf &out) {
    PrintBoxHeader(out, title);
    static constexpr Column SUB_HELP_COLS[] = {
        {"Subcommand / Syntax", 34, Align::LEFT, Align::LEFT},
        {"Description", 39, Align::LEFT, Align::LEFT},
    };
    TableRenderer table(out, SUB_HELP_COLS, 2);
    table.header(false);
    for (size_t i = 0; i < count; ++i) {
      if (defs[i].syntax)
        table.row({defs[i].syntax, defs[i].desc ? defs[i].desc : ""});
    }
    table.end('-');
    if (tip) {
      PrintBoxFooter(out, tip);
    } else {
      out.append(BOX80_EQ);
      out.append("\r\n");
    }
  });
}

inline bool DispatchSubCmd(const char *sub, int sock, int argc,
                           const Args &args, const SubCmdDef *defs,
                           size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (defs[i].handler && defs[i].name && strcasecmp(sub, defs[i].name) == 0) {
      defs[i].handler(sock, argc, args);
      return true;
    }
  }
  return false;
}

inline void PrintSubCmdHelp(int sock, const char *title,
                            std::span<const SubCmdDef> defs,
                            const char *tip = nullptr) {
  PrintSubCmdHelp(sock, title, defs.data(), defs.size(), tip);
}

inline bool DispatchSubCmd(const char *sub, int sock, int argc,
                           const Args &args, std::span<const SubCmdDef> defs) {
  return DispatchSubCmd(sub, sock, argc, args, defs.data(), defs.size());
}

} // namespace CliFmt

struct HwSnapshot;
struct PktSnapshot;
struct LatencySnapshot;

namespace Fmt {
void FormatHwMetrics(AppendBuf &out, const HwSnapshot &hw);
void FormatNetworkStats(AppendBuf &out, const PktSnapshot &pkt);
void FormatRs485Stats(AppendBuf &out, const PktSnapshot &pkt);
void FormatCh1Latency(AppendBuf &out, const LatencySnapshot &lat);
} // namespace Fmt

// ── CLI Subsystem Command Interfaces ────────────────────────────────────────
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
void cmdSup(CliContext &ctx);
void cmdReboot(CliContext &ctx);
void cmdLogView(CliContext &ctx);
void cmdCoreDump(CliContext &ctx);
void cmdNvs(CliContext &ctx);
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

// ── Unified Command Table Definition ────────────────────────────────────────
struct CommandDef {
  const char *name;
  void (*handler)(CliContext &ctx);
  uint8_t min_args;
  const char *help;
};

extern const CommandDef kConsoleCmds[];
extern const size_t kConsoleCmdsCount;
