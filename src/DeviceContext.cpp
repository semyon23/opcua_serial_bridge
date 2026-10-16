
#include "../include/DeviceContext.hpp"
#include <spdlog/spdlog.h>


DeviceContext::DeviceContext(Config cfg) : config(cfg) {
    auto base_path = std::vformat(cfg.opc_path_pattern, std::make_format_args(config.port_index, config.device_index));
    cmd_ready_node = opcua::NodeId::parse(base_path + ".cmdReady");
    info_ready_node = opcua::NodeId::parse(base_path + ".infoReady");
    control_nodes.reserve(8);
    control_nodes.push_back(opcua::NodeId::parse(base_path + ".control.command"));
    control_nodes.push_back(opcua::NodeId::parse(base_path + ".control.transaction_id"));
    control_nodes.push_back(opcua::NodeId::parse(base_path + ".control.powerOn"));
    control_nodes.push_back(opcua::NodeId::parse(base_path + ".control.axisId"));
    control_nodes.push_back(opcua::NodeId::parse(base_path + ".control.position"));
    control_nodes.push_back(opcua::NodeId::parse(base_path + ".control.velocity"));
    control_nodes.push_back(opcua::NodeId::parse(base_path + ".control.acceleration"));
    control_nodes.push_back(opcua::NodeId::parse(base_path + ".control.direction"));
    status_nodes.reserve(8);
    status_nodes.push_back(opcua::NodeId::parse(base_path + ".info.transaction_id"));
    status_nodes.push_back(opcua::NodeId::parse(base_path + ".info.state"));
    status_nodes.push_back(opcua::NodeId::parse(base_path + ".info.isHome"));
    status_nodes.push_back(opcua::NodeId::parse(base_path + ".info.isTarget"));
    status_nodes.push_back(opcua::NodeId::parse(base_path + ".info.isPowered"));
    status_nodes.push_back(opcua::NodeId::parse(base_path + ".info.inMotion"));
    status_nodes.push_back(opcua::NodeId::parse(base_path + ".info.errorID"));
    status_nodes.push_back(opcua::NodeId::parse(base_path + ".info.position"));
}

uint8_t DeviceContext::get_device_index() const {
    return config.device_index;
}

uint8_t DeviceContext::get_port_index() const {
    return config.port_index;
}

const opcua::NodeId &DeviceContext::get_cmd_ready_node() const {
    return cmd_ready_node;
}

const opcua::NodeId &DeviceContext::get_info_ready_node() const {
    return info_ready_node;
}

const std::vector<opcua::NodeId> &DeviceContext::get_control_nodes() const {
    return control_nodes;
}

const std::vector<opcua::NodeId> &DeviceContext::get_status_nodes() const {
    return status_nodes;
}

SerialFrame<CommandData> DeviceContext::make_status_request() {
    SerialFrame<CommandData> cmd_status;
    cmd_status.device_id = config.device_index;
    cmd_status.cmd_id = 10;
    return cmd_status;
}

std::optional<SerialFrame<CommandData> > DeviceContext::opc_to_serial_struct(std::span<const opcua::Variant> opc_data) {
    if (opc_data.size() != control_nodes.size()) {
        spdlog::error("[DEV:{}:{}] Error: insufficient data ", config.port_index, config.device_index);
        return std::nullopt;
    }
    SerialFrame<CommandData> cmd{};

    try {
        cmd.cmd_id = opc_data[0].to<uint8_t>();
        cmd.data.transaction_id = opc_data[1].to<uint8_t>();
        cmd.data.powerOn = opc_data[2].to<bool>();
        cmd.data.axis_id = opc_data[3].to<uint8_t>();
        if (auto pos = opc_data[4].to<std::vector<float>>(); pos.size() >= 3) {
            std::copy_n(pos.begin(), 3, cmd.data.position.begin());
        }
        cmd.data.velocity = opc_data[5].to<float>();
        cmd.data.acceleration = opc_data[6].to<float>();
        cmd.data.deceleration = opc_data[6].to<float>();
        if (auto dir = opc_data[7].to<std::vector<int8_t>>(); dir.size() >= 3) {
            std::copy_n(dir.begin(), 3, cmd.data.direction.begin());
        }
        auto now = std::chrono::system_clock::now();
        spdlog::info("[DEV:{}:{}] CMD:{} TR_ID:{} AXIS:{} PWR:{} X:{:.2f} Y:{:.2f} Z:{:.2f} Vel:{:.2f} Acc:{:.2f} Dec:{:.2f} Dir:{},{},{}",
            config.port_index,
            config.device_index,
            cmd.cmd_id,
            cmd.data.transaction_id,
            cmd.data.axis_id,
            cmd.data.powerOn,
            cmd.data.position[0],
            cmd.data.position[1],
            cmd.data.position[2],
            cmd.data.velocity,
            cmd.data.acceleration,
            cmd.data.acceleration,
            cmd.data.direction[0],
            cmd.data.direction[1],
            cmd.data.direction[2]);
        return cmd;
    } catch (const std::exception& e) {
        spdlog::critical( "Exeption : {}", e.what() );
        return std::nullopt;
    }
}

std::vector<opcua::Variant> DeviceContext::serial_struct_to_opc(const StatusData &serial_data) {
    std::vector<opcua::Variant> values;
    values.reserve(status_nodes.size());
    values.push_back(opcua::Variant(serial_data.transaction_id));
    values.push_back(opcua::Variant(serial_data.state_id));
    values.push_back(opcua::Variant(serial_data.is_home));
    values.push_back(opcua::Variant(serial_data.is_target));
    values.push_back(opcua::Variant(serial_data.is_powered));
    values.push_back(opcua::Variant(serial_data.in_motion));
    values.push_back(opcua::Variant(serial_data.error_id));
    values.push_back(opcua::Variant(serial_data.position));

    // spdlog::info("[DEV:{}:{}] TR_ID:{}",
    //        config.port_index,
    //        config.device_index,
    //        serial_data.transaction_id);
    return values;
}
