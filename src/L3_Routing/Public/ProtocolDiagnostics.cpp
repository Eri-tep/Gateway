// ============================================================================
// ProtocolDiagnostics: Level 3 Public Diagnostics & Engine Facade Implementation
// ============================================================================

#include "L3_Routing/Public/ProtocolDiagnostics.h"
#include "L3_Routing/Private/Wallpad_Protocol.h"
#include <span>

void ProtocolDiag_WarmCacheSaveToNvs() noexcept {
  WarmCache_SaveToNvs();
}

void ProtocolDiag_WarmCacheCheckNvsDebounce() noexcept {
  WarmCache_CheckNvsDebounce();
}

void ProtocolDiag_PollingResetHits() noexcept {
  g_polling_targets.resetHits();
}

void ProtocolDiag_PollingClear() noexcept {
  g_polling_targets.clear();
}

void ProtocolDiag_PollingRegisterOrTouch(uint8_t ch, uint8_t dev_id, uint8_t sub1,
                                         uint8_t sub2, const uint8_t *pkt_data,
                                         size_t pkt_len) noexcept {
  g_polling_targets.registerOrTouch(ch, dev_id, sub1, sub2, pkt_data, pkt_len);
}

void ProtocolDiag_AutoProbingReset() noexcept {
  g_auto_probing_engine.reset();
}

void ProtocolDiag_AutoProbingFeedFrame(const uint8_t *data, size_t len) noexcept {
  if (data && len > 0) {
    g_auto_probing_engine.feedFrame(std::span<const uint8_t>(data, len));
  }
}

void ProtocolDiag_GetFramingNamespace(uint8_t profile_idx, char *out_buf,
                                      size_t buf_len) noexcept {
  FramingTracker::getNvsNamespace(profile_idx, out_buf, buf_len);
}

bool ProtocolDiag_ExtractDeviceKey(const uint8_t *data, size_t len,
                                   uint8_t &out_dev_id, uint8_t &out_sub1,
                                   uint8_t &out_sub2) noexcept {
  if (!data || len < 5)
    return false;
  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser)
    return false;
  std::span<const uint8_t> frame(data, len);
  parser->extractDeviceKey(frame, out_dev_id, out_sub1, out_sub2);
  return true;
}

void ProtocolDiag_GetActiveVendorName(char *out_buf, size_t max_len) noexcept {
  if (!out_buf || max_len == 0)
    return;
  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    parser->getVendorName(out_buf, max_len);
  } else {
    snprintf(out_buf, max_len, "Unknown");
  }
}

void ProtocolDiag_GetProfileSummary(char *out_buf, size_t max_len) noexcept {
  if (!out_buf || max_len == 0)
    return;

  auto *active = WallpadParserFactory::getActiveParser();
  auto desc = g_auto_probing_engine.getDescriptor();
  char vendor_name_buf[64] = "Unknown";
  if (active) {
    active->getVendorName(vendor_name_buf, sizeof(vendor_name_buf));
  }
  const char *catalog_vendor = vendor_name_buf;

  if (g_config.wallpad_profile == 0) {
    snprintf(out_buf, max_len,
             desc.is_locked ? "Auto Detect (%s)" : "Auto Detect (Learning...)",
             catalog_vendor);
  } else {
    VendorProfileDescriptor cur_p;
    const char *p_name = ProfileRepository::getActiveProfile(cur_p)
                             ? (cur_p.name[0] ? cur_p.name : cur_p.key)
                             : nullptr;
    if (p_name)
      snprintf(out_buf, max_len, "%s (%s)", p_name, catalog_vendor);
    else
      snprintf(out_buf, max_len, "%s", catalog_vendor);
  }
}

