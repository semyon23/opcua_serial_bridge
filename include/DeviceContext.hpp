
#ifndef OPCSERIALBRIDGE_DEVICECONTEXT_HPP
#define OPCSERIALBRIDGE_DEVICECONTEXT_HPP

#include "OpcSerialBridge.hpp"

#include <string>

struct Config {
    uint8_t port_index{1};
    uint8_t device_index{1};
    std::string opc_path_pattern{""};
};

class DeviceContext {
public:
    DeviceContext(Config cfg);
    std::optional<SerialFrame<CommandData>> opc_to_serial_struct(std::span<const opcua::Variant> opc_data);
    std::vector<opcua::Variant> serial_struct_to_opc(const StatusData& serial_data);

    SerialFrame<CommandData> make_status_request();
    uint8_t get_device_index() const;
    uint8_t get_port_index() const;

    const opcua::NodeId& get_cmd_ready_node() const;
    const opcua::NodeId& get_info_ready_node() const;
    const std::vector<opcua::NodeId>& get_control_nodes() const;
    const std::vector<opcua::NodeId>& get_status_nodes() const;
private:
    Config config;


    opcua::NodeId cmd_ready_node;
    opcua::NodeId info_ready_node;
    std::vector<opcua::NodeId> control_nodes;
    std::vector<opcua::NodeId> status_nodes;
};


#endif //OPCSERIALBRIDGE_DEVICECONTEXT_HPP
