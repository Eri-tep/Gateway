#include "Common.h"
#include "TelnetCli.h"
#include "WallpadParser.h"
#include "ControlTemplate.h"
#include <cstring>
#include <algorithm>

DeviceRouteRegistry g_route_registry;

void DeviceRouteRegistry::recordRoute(uint8_t channel_id, int8_t slot_idx,
                                      uint8_t dev_id, uint8_t sub1,
                                      uint8_t sub2) {
  CriticalSectionLocker lock(&_mux);
  uint32_t now = millis();

  for (size_t i = 0; i < _count; i++) {
    if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
        _entries[i].sub2 == sub2) {
      _entries[i].endpoint.channel_id = channel_id;
      _entries[i].endpoint.slot_idx = slot_idx;
      _entries[i].endpoint.last_seen_ms = now;
      return;
    }
  }

  if (_count < MAX_ROUTES) {
    _entries[_count].dev_id = dev_id;
    _entries[_count].sub1 = sub1;
    _entries[_count].sub2 = sub2;
    _entries[_count].endpoint.channel_id = channel_id;
    _entries[_count].endpoint.slot_idx = slot_idx;
    _entries[_count].endpoint.last_seen_ms = now;
    _count++;
  }
}

bool DeviceRouteRegistry::lookupRoute(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                      RouteEndpoint &out_ep) const {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; i++) {
    if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
        _entries[i].sub2 == sub2) {
      out_ep = _entries[i].endpoint;
      return true;
    }
  }
  return false;
}

size_t DeviceRouteRegistry::getRoutes(DeviceRouteEntry *out_buf,
                                      size_t max_count) const {
  CriticalSectionLocker lock(&_mux);
  size_t copy_cnt = std::min(_count, max_count);
  for (size_t i = 0; i < copy_cnt; i++) {
    out_buf[i] = _entries[i];
  }
  return copy_cnt;
}

void DeviceRouteRegistry::clear() {
  CriticalSectionLocker lock(&_mux);
  _count = 0;
  memset(_entries, 0, sizeof(_entries));
}


bool ControlDispatcher::dispatch(StaticPacket &req,
                                 StaticPacket &virtual_ack_out) {
  if (UNLIKELY(req.length < 5))
    return false;
  auto *parser = WallpadParserFactory::getActiveParser();
  span<const uint8_t> frame(req.data.data(), req.length);
  if (parser->isQueryPacket(frame)) {
    virtual_ack_out.channel_id = req.channel_id;
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (!parser->extractDeviceKey(frame, dev_id, sub1, sub2)) {
      return false;
    }
    return g_device_repo.copyVirtualAck(dev_id, sub1, sub2, virtual_ack_out);
  }

  bool is_ctl = parser->isControlPacket(frame);
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  bool has_key = parser->extractDeviceKey(frame, dev_id, sub1, sub2);
  const GroupControlTemplate *grp = (has_key && dev_id != 0) ? g_control_registry.findGroup(dev_id) : nullptr;

  // [무사전지식] 프로파일 기본 ctrl_op(0x02 등)뿐만 아니라,
  // 런타임에 청사진으로 학습된 패킷의 Opcode(엘리베이터 0x04 등)도 제어 패킷으로 인식
  if (!is_ctl && grp && grp->frame_len > 4 && frame.size() >= grp->frame_len) {
    VendorProfileDescriptor desc;
    ProfileRepository::getActiveProfile(desc);
    uint8_t op_off = (desc.opcode_offset < frame.size()) ? desc.opcode_offset : 4;
    if (frame[op_off] == grp->raw_template[op_off]) {
      is_ctl = true;
    }
  }

  if (is_ctl) {
    // [보안 4.2] 외부/CH6 제어 패킷 유효 범위 검증 및 인젝션/가스열기 방어
    if (grp) {
      // 1) 가스 밸브 열기 방어: GAS 장치에 대해 close 토큰이 아닌 값이 주입되면 차단
      if (grp->coverage.dev_class == DeviceClass::GAS) {
        if (grp->close_slot.discovered && grp->close_slot.action_offset < req.length) {
          uint8_t val = req.data[grp->close_slot.action_offset];
          if (val != grp->close_slot.off_val) {
            g_telnet_tracer.trace(req.channel_id, false, TraceType::DRP, req);
            return false;
          }
        }
      }
      // 2) 난방 온도 범위 방어: 희망온도(SET_TEMP) 카테고리 패킷일 때만 온도 유효 범위(5~35C) 검증
      if (grp->coverage.dev_class == DeviceClass::THERMOSTAT && grp->temp_slot.discovered &&
          grp->temp_slot.action_offset < req.length) {
        bool is_temp = false;
        // 카테고리 슬롯이 학습되어 있다면 temp_slot의 카테고리 값과 일치할 때만 온도 패킷으로 판정
        if (grp->temp_slot.category_offset != 0xFF && grp->temp_slot.category_offset < req.length) {
          is_temp = (req.data[grp->temp_slot.category_offset] == grp->temp_slot.category_val);
        }

        if (is_temp) {
          uint8_t t_val = req.data[grp->temp_slot.action_offset];
          if (t_val < 5 || t_val > 35) {
            g_telnet_tracer.trace(req.channel_id, false, TraceType::DRP, req);
            return false;
          }
        }
      }
    }

    RouteEndpoint ep{1, -1, 0};
    bool route_known = false;
    if (has_key) {
      route_known = g_route_registry.lookupRoute(dev_id, sub1, sub2, ep);
    }

    // [동적 라우팅] 학습된 경로가 CH5(EW11)인 경우 해당 EW11 TCP 소켓으로 직접 인젝션 송신
    if (route_known && ep.channel_id == 5 && ep.slot_idx >= 0 && ep.slot_idx < Config::TCP::MAX_EW11_SLOTS) {
      auto &sl = g_ew11_slots[ep.slot_idx];
      sl.last_ctrl_len = static_cast<uint8_t>(std::min<size_t>(req.length, sizeof(sl.last_ctrl_data)));
      memcpy(sl.last_ctrl_data, req.data.data(), sl.last_ctrl_len);
      sl.last_ctrl_tx_ms = millis();

      bool sent = Ew11_SendPacket(static_cast<uint8_t>(ep.slot_idx), req);
      g_telnet_tracer.trace(5, true, sent ? TraceType::CTL : TraceType::DRP, req);
      return false;
    }

    // [기본 라우팅] CH1(물리 RS-485 버스)
    QueueHandle_t q = (req.channel_id == 6) ? g_ch1_vip_queue : g_ch1_control_queue;
    if (!Queue_EnqueueDropHead(q, req)) {
      return false;
    }
    g_telnet_tracer.trace(1, true, TraceType::CTL, req);
    return false;
  }
  return false;
}
