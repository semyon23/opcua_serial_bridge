#pragma once

#include "protocol.hpp"

#include <open62541pp/open62541pp.hpp>

#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

struct NodeOptions;

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
    CtrlDeceleration,          // только если в конфиге deceleration_node: true
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
    std::optional<opcua::NodeId> cmd_result;    // result_node: true
    std::optional<opcua::NodeId> online;        // online_node: true
    std::vector<opcua::NodeId> control;         // по ControlNode
    std::vector<opcua::NodeId> status;          // по StatusNode

    static DeviceNodes make(const NodeOptions& options, uint16_t port_index, uint16_t device_index);
};

// Итог команды для узла cmdResult.
enum class CommandResult : uint8_t {
    Ok = 0,             // плата приняла кадр и ответила
    ReadFailed = 1,     // не прочитались узлы control.*
    Rejected = 2,       // недопустимая команда или значения
    NoReply = 3,        // плата не ответила
};

struct PlcCommand {
    uint8_t cmd{0};
    proto::CommandData data;
};

// Значения control.* → кадр команды. Ошибка — текст для журнала.
// Разрешены только команды движения и сервиса: CHANGE_ID и JUMP_TO_BOOTLOADER из ПЛК не проходят.
std::variant<PlcCommand, std::string> decode_command(std::span<const opcua::Variant> values, bool has_deceleration);

// StatusData → значения info.* в порядке StatusNode.
std::vector<opcua::Variant> encode_status(const proto::StatusData& status);
