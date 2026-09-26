#pragma once

// ============================================================================
// GATEWAY UMBRELLA HEADER (Facade)
// ============================================================================
// 기존 모든 소스코드와의 100% 하위 호환성을 유지하기 위한 Umbrella 헤더입니다.
// 역할별 도메인 헤더를 계층 순서대로 포함합니다:
//   1. Platform.h : 시스템 include, CoreDump, FastCrc32, NvsEnvelope, HexLUT, TimeUtils
//   2. Config.h   : 통신속도, 버퍼크기, 타이밍, 도어폰 프로파일, RuntimeConfig
//   3. Buffers.h  : Fmt, AppendBuf, Tcp_IsAllowedIP
//   4. Metrics.h  : MutexLocker, CriticalSectionLocker, PacketStatistics, SystemMetricsTracker
//   5. Devices.h  : StaticPacket, DeviceStateEntry, DeviceRepository, Task/Queue externs

#include "core/Platform.h"
#include "core/Config.h"
#include "core/Buffers.h"
#include "core/Metrics.h"
#include "core/Devices.h"