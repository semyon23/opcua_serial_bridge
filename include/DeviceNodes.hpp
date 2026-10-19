#pragma once

#include "protocol.hpp"

#include <open62541pp/open62541pp.hpp>

#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// Порядок узлов control.* в запросе Read. Индексы — только через это перечисление.
enum ControlNode : size_t {
    CtrlCommand,
    CtrlTransactionId,
    CtrlPowerOn,
    CtrlAxisId,
    CtrlPosition,
    CtrlVelocity,
    CtrlAcceleration,
    CtrlDirection,
    ControlNodeCount,
};

// Порядок узлов info.* в запросе Write — как в текущем мосте.
enum StatusNode : size_t {
    StTransactionId,
    StState,
    StIsHome,
    StIsTarget,
    StIsPowered,
    StInMotion,
    StErrorId,
    StPosition,
    StatusNodeCount,
};

// Все узлы ПЛК одного устройства: база из шаблона + суффиксы.
struct DeviceNodes {
    std::string base;
    opcua::NodeId cmd_ready;
    opcua::NodeId info_ready;
    opcua::NodeId online;                       // мост пишет: отвечает ли плата
    std::vector<opcua::NodeId> control;         // по ControlNode
    std::vector<opcua::NodeId> status;          // по StatusNode

    static DeviceNodes make(const std::string& pattern, uint16_t port_index, uint16_t device_index);
};

struct PlcCommand {
    uint8_t cmd{0};
    proto::CommandData data;
};

// Значения control.* → кадр команды. Ошибка — текст для журнала.
// Разрешены только команды движения и сервиса: CHANGE_ID и JUMP_TO_BOOTLOADER из ПЛК не проходят.
// Торможение = ускорению: отдельного узла торможения в ПЛК нет.
std::variant<PlcCommand, std::string> decode_command(std::span<const opcua::Variant> values);

// StatusData → значения info.* в порядке StatusNode.
std::vector<opcua::Variant> encode_status(const proto::StatusData& status);
