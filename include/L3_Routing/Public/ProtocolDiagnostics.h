#pragma once

// ============================================================================
// ProtocolDiagnostics: Level 3 Public Diagnostics & Engine Facade
// ============================================================================
// Provides thread-safe, decoupled diagnostic queries and management operations
// for L4 Services (CLI Console, Remote JSON-RPC, Telemetry) without leaking
// L3 Private internal singletons or raw packet codec engines.
// ============================================================================

#include "L0_Base/System_Buffer.h"
#include <cstddef>
#include <cstdint>

// ── Cache & NVS Persistence Facade ──────────────────────────────────────────
void ProtocolDiag_WarmCacheSaveToNvs() noexcept;
void ProtocolDiag_WarmCacheCheckNvsDebounce() noexcept;

// ── Polling & Auto-Probing Management Facade ────────────────────────────────
void ProtocolDiag_PollingResetHits() noexcept;
void ProtocolDiag_PollingClear() noexcept;
void ProtocolDiag_PollingRegisterOrTouch(uint8_t ch, uint8_t dev_id, uint8_t sub1,
                                         uint8_t sub2, const uint8_t *pkt_data,
                                         size_t pkt_len) noexcept;
void ProtocolDiag_AutoProbingReset() noexcept;
void ProtocolDiag_AutoProbingFeedFrame(const uint8_t *data, size_t len) noexcept;

// ── Framing & Profile Helpers ────────────────────────────────────────────────
void ProtocolDiag_GetFramingNamespace(uint8_t profile_idx, char *out_buf,
                                      size_t buf_len) noexcept;
bool ProtocolDiag_ExtractDeviceKey(const uint8_t *data, size_t len,
                                   uint8_t &out_dev_id, uint8_t &out_sub1,
                                   uint8_t &out_sub2) noexcept;
void ProtocolDiag_GetActiveVendorName(char *out_buf, size_t max_len) noexcept;
void ProtocolDiag_GetProfileSummary(char *out_buf, size_t max_len) noexcept;

