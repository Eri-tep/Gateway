#pragma once

#include "core/WallpadProfile.h"
#include "WallpadParser.h"

class ControlTemplateRegistry;

namespace ProfileMatcher {

// 현재 수렴/잠금된 AutoProbeDescriptor를 기반으로 일치하는 제조사 프로파일을 검색합니다.
const WallpadProfile *matchProfile(const AutoProbeDescriptor &ad);

// 현재 활성화/매칭된 프로파일 반환 (기본값: kHyundaiProfile)
const WallpadProfile *getActiveProfile();

// 도어폰 패킷 헤더 매칭
const DoorphoneSpec *matchDoorphone(uint8_t stx, uint8_t etx, uint8_t len);

// 특정 제조사 프로파일의 기기 명세(DeviceSpec)를 ControlTemplateRegistry에 주입합니다.
void injectProfile(const WallpadProfile *profile, ControlTemplateRegistry &registry);

// 캐시 수렴 시 호출되는 원스톱 엔트리포인트 (매칭 후 자동 슬롯 주입)
void matchAndInject(const AutoProbeDescriptor &ad, ControlTemplateRegistry &registry);

} // namespace ProfileMatcher
