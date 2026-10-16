
#include "../include/OpcSerialBridge.hpp"
#include <spdlog/spdlog.h>
#include "../include/DeviceContext.hpp"

OpcSerialBridge::OpcSerialBridge(asio::io_context &ioc, std::shared_ptr<OpcTransport> opc)
    : ctx(ioc), opc_client(opc) {

    auto strKeepAlive =  "ns=4;s=|var|Inovance-PLC.Application.gvlPnp.keepAlive";
    keep_alive_node = opcua::NodeId::parse(strKeepAlive);
}

void OpcSerialBridge::add_serial_port(std::shared_ptr<SerialTransport> port) {
    if (!port) {
        spdlog::error("[BRIDGE] Attempted to add a null serial port pointer.");
        return;
    }
    serial_ports.push_back(port);
    spdlog::info("[BRIDGE] Registered port ID: {} ", port->get_port_id());
}

void OpcSerialBridge::init(){
    spdlog::info("[BRIDGE] Initializing manipulator devices and OPC UA subscriptions...");

    for (uint8_t port = 1; port <= serial_ports.size(); ++port){
        for (uint8_t id = 1; id <= 3; ++id){
            Config cfg{port, id, "ns=4;s=|var|Inovance-PLC.Application.gvlPnp.ManipulatorPorts[{}].Manipulator[{}]"};

            auto device = std::make_shared<DeviceContext>(cfg);
            device_router[{port, id}] = device;

            opc_client->register_node(device->get_cmd_ready_node(), [this, port, id](const opcua::Variant& value){
                if (value.empty() || !value.to<bool>()) return;

                asio::post(ctx, [this, port, id](){
                    auto it = this->device_router.find({port, id});
                    if (it != this->device_router.end()){
                        this->handle_plc_command(port, it->second);
                    }
                });
            });
            // opc_client->register_node(device->get_cmd_ready_node(), [this, port, id](const opcua::Variant& value) {
            //     if (value.empty() || !value.to<bool>()) return;
            //
            //     asio::post(ctx, [this, port, id]() {
            //         auto it = this->device_router.find({port, id});
            //         if (it != this->device_router.end()) {
            //             this->handle_plc_command(port, it->second);
            //         }
            //     });
            // });
        }
    }

    spdlog::info("[BRIDGE] Successfully initialized {} devices in router map.", device_router.size());
}

void OpcSerialBridge::run() {
    init();
    opc_client->run();
    for (const auto& serial_port : serial_ports) {
        serial_port->run();
        asio::co_spawn(ctx, process_port_responses(serial_port), asio::detached);
    }
    asio::co_spawn(ctx, schedule_send_keep_alive(), asio::detached);
    asio::co_spawn(ctx, schedule_send_status_data(), asio::detached);
}

void OpcSerialBridge::stop() {
    opc_client->stop();
    for (const auto& serial_port : serial_ports) {
        serial_port->stop();
    }
}

void OpcSerialBridge::batch_reading_nodes(const std::vector<opcua::NodeId> &nodes, std::function<void(std::vector<opcua::Variant>)> cb) {
    if (nodes.empty() || !cb) return;

    auto results = std::make_shared<std::vector<opcua::Variant>>(nodes.size());

    auto latch = create_async_latch(nodes.size(), [this, results, final_cb = std::move(cb)]() {
        asio::post(this->ctx, [results, cb_moved = std::move(final_cb)]() {
            cb_moved(std::move(*results));
        });
    });

    for (size_t i = 0; i < nodes.size(); ++i) {
        opc_client->read_from_node(nodes[i], [results, i, latch](const opcua::NodeId&, opcua::Variant value) {
            (*results)[i] = std::move(value);
            latch();
        });
    }
}

void OpcSerialBridge::handle_plc_command(uint8_t port, std::shared_ptr<DeviceContext> dev_context) {
    this->batch_reading_nodes(dev_context->get_control_nodes(), [this, port, dev_context](std::vector<opcua::Variant> opc_results) {

        auto frame_opt = dev_context->opc_to_serial_struct(opc_results);
        if (!frame_opt) {
            spdlog::error("[BRIDGE] [DEV:{}:{}] Failed to map OPC variables to SerialFrame.",
                          port, dev_context->get_device_index());
            return;
        }
        auto frame = frame_opt.value();
        frame.device_id = dev_context->get_device_index();
        std::vector<uint8_t> binary_data = data_parser.struct_to_binary(frame);

        this->push_to_serial(port, dev_context->get_device_index(), binary_data);
        this->opc_client->write_to_node(dev_context->get_cmd_ready_node(), opcua::Variant(false), {});
    });
}

void OpcSerialBridge::send_info_to_opc(const std::vector<opcua::NodeId> &nodes, std::vector<opcua::Variant> &&values, const opcua::NodeId &info_ready) {
    if (nodes.size() != values.size() || nodes.empty()) return;

    auto latch = create_async_latch(nodes.size(), [this, info_ready]() {
        opc_client->write_to_node(info_ready, opcua::Variant(true), {});
    });

    for (size_t i = 0; i < nodes.size(); ++i) {
        opc_client->write_to_node(nodes[i], values[i], [latch]() {
            latch();
        });
    }
}

asio::awaitable<void> OpcSerialBridge::process_port_responses(std::shared_ptr<SerialTransport> serial_port) {
    const uint8_t port_id = serial_port->get_port_id();

    while (true) {
        auto response_opt = co_await serial_port->get_response();

        if (!response_opt.has_value()) {
            spdlog::info("[BRIDGE] Response channel for port {} closed gracefully.", port_id);
            co_return;
        }

        auto& response = response_opt.value();

        if (response.port_id != port_id) {
            spdlog::error("[BRIDGE] Port ID does not match {}", response.port_id);
            continue;
        }

        auto frame = data_parser.binary_to_struct(response.data);
        if (!frame) {
            spdlog::error("[BRIDGE] Failed to parse packet from port {}", response.port_id);
            continue;
        }

        auto it = device_router.find({port_id, response.device_id});
        if (it != device_router.end()) {
            auto& device = it->second;

            auto opc_values = device->serial_struct_to_opc(frame.value().data);

            send_info_to_opc(device->get_status_nodes(), std::move(opc_values), device->get_info_ready_node());
        } else {
            spdlog::error("[BRIDGE] Unknown device {} on port {}", response.device_id, response.port_id);
        }
    }
}

asio::awaitable<void> OpcSerialBridge::schedule_send_keep_alive() {
    uint32_t counter = 0;
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(std::chrono::milliseconds(3000));
    while (true) {
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));

        if (ec) {
            spdlog::error("[{}] Keep-alive timer error: {}", name, ec.message());
            co_return;
        }
        opc_client->write_to_node(keep_alive_node, opcua::Variant(counter++), {});
        timer.expires_after(std::chrono::milliseconds(1000));
    }
}

asio::awaitable<void> OpcSerialBridge::schedule_send_status_data() {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(std::chrono::milliseconds(3000));
    while (true) {
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));

        if (ec) {
            spdlog::error("[{}] Send status timer error: {}", name, ec.message());
            co_return;
        }
        for (const auto& [key, device] : device_router) {
            SerialFrame<CommandData> cmd = device->make_status_request();
            std::vector<uint8_t> binary_data = data_parser.struct_to_binary(cmd);
            this->push_to_serial(device->get_port_index(), device->get_device_index(), binary_data);
        }
        timer.expires_at(timer.expiry() + std::chrono::milliseconds(100));
    }
}

void OpcSerialBridge::push_to_serial(uint8_t port, uint8_t device, const std::vector<uint8_t> &frame) {
    for (const auto& serial_port : serial_ports) {

        if (serial_port && serial_port->get_port_id() == port) {
            SerialTransaction transaction{frame, sizeof(SerialFrame<StatusData>), device};
            serial_port->push_request(transaction);
            return;
        }
    }
    spdlog::error( "[BRIDGE] Error: Serial port ID: {} - no found ", port);
}
