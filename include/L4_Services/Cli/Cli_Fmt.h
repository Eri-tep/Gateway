#pragma once

#include "L4_Services/CLI_Service.h"
#include "L4_Services/Cli/Cli_Commands.h"
#include <climits>
#include <cstdlib>
#include <cstring>
#include <functional>

// Shared scratch buffer for CLI output rendering (single allocation in
// ConsoleFmt.cpp)
char *Cli_GetScratchBuffer();
size_t Cli_GetScratchBufferSize();

// ============================================================================
// CLI 80-COLUMN UNIFIED FORMATTING & BUFFER HELPERS
// ============================================================================

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

// Unified subcommand descriptor: carries dispatch handler AND help strings.
// handler == nullptr marks a help-only (separator / header) row.
struct SubCmdDef {
  const char *name;   // token matched against args.get(1), e.g. "frame"
  const char *syntax; // help table left column
  const char *desc;   // help table right column
  void (*handler)(int sock, int argc,
                  const Args &args); // nullptr = help row only
};

} // namespace CliFmt

// Shared scratch buffer executor
void withScratchBufInternal(int sock, std::function<void(AppendBuf &)> fn);

template <typename F> inline void withScratchBuf(int sock, F &&fn) {
  withScratchBufInternal(sock, std::forward<F>(fn));
}

namespace CliFmt {

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

// Dispatch: scan defs[], call matching handler. Returns true if matched.
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

} // namespace CliFmt

namespace Fmt {
void FormatHwMetrics(AppendBuf &out, const HwSnapshot &hw);
void FormatNetworkStats(AppendBuf &out, const PktSnapshot &pkt);
void FormatRs485Stats(AppendBuf &out, const PktSnapshot &pkt);
} // namespace Fmt
