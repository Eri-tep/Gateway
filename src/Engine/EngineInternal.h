#pragma once

#include "Common.h"
#include "MgmtRpc.h"
#include "WallpadParser.h"
#include "ControlTemplate.h"
#include "TelnetCli.h"

enum class UartRxStatus { SUCCESS, TIMEOUT };
using UartPollCallback = void (*)(void *ctx);

QueueHandle_t Uart_GetEventQueue(uart_port_t u_num);

UartRxStatus Uart_RecvPacket(uart_port_t u_num, StaticPacket &out,
                            uint32_t tout_ms,
                            UartPollCallback on_poll = nullptr,
                            void *poll_ctx = nullptr,
                            const StaticPacket *echo_match = nullptr);

void Ch1_WaitBusIdle(uint32_t silence_ms);
void Ch1_HandleCtrl(const StaticPacket &ctrlPacket);
void Ch1_PollNext(size_t &current_dev_idx);
void Ch1_SetState(Ch1State &cur_state, Ch1State new_state);

namespace PacketBuilder {
void Ch1_BuildQueryPacket(StaticPacket &out, uint8_t dev_id, uint8_t sub1,
                          uint8_t sub2);
}
