
#ifndef OPCSERIALBRIDGE_OPCSERIALBRIDGE_HPP
#define OPCSERIALBRIDGE_OPCSERIALBRIDGE_HPP
#pragma once
#include "SerialTransport.hpp"
#include "OpcTransport.hpp"
#include "utils/DataParser.hpp"
//#include "DeviceContext.hpp"

class DeviceContext;

class OpcSerialBridge {
public:
    OpcSerialBridge(asio::io_context &ioc, std::shared_ptr<OpcTransport> opc);
    void run();
    void stop();
    void add_serial_port(std::shared_ptr<SerialTransport> port);
private:
    void init();
    void push_to_serial(uint8_t port, uint8_t device, const std::vector<uint8_t>& frame);
    void send_info_to_opc(const std::vector<opcua::NodeId>& nodes, std::vector<opcua::Variant>&& values,  const opcua::NodeId& info_ready);
    void batch_reading_nodes(const std::vector<opcua::NodeId>& nodes, std::function<void(std::vector<opcua::Variant>)> cb);
    void handle_plc_command(uint8_t port, std::shared_ptr<DeviceContext> dev_context);
    asio::awaitable<void> process_port_responses(std::shared_ptr<SerialTransport> serial_port);
    asio::awaitable<void> schedule_send_keep_alive();
    asio::awaitable<void> schedule_send_status_data();

    asio::io_context &ctx;
    std::shared_ptr<OpcTransport> opc_client;
    std::vector<std::shared_ptr<SerialTransport>> serial_ports;


    auto create_async_latch(size_t count, std::function<void()>&& on_complete) {
        return [remaining = std::make_shared<std::atomic<size_t>>(count),
                action = std::move(on_complete)]() {
            if (remaining->fetch_sub(1) == 1) {
                action();
            }
        };
    }

    std::map<std::pair<uint8_t, uint8_t>, std::shared_ptr<DeviceContext>> device_router;

    opcua::NodeId keep_alive_node;
    DataParser data_parser;
    std::string name{"BRIDGE"};
};


#endif //OPCSERIALBRIDGE_OPCSERIALBRIDGE_HPP
