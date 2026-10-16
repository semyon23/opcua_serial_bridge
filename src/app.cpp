#include <iostream>
#include "../include/Logger.hpp"
#include <spdlog/spdlog.h>
#include "../include/OpcSerialBridge.hpp"
#include "../include/config_loader.hpp"

int main() {
    if (!Logger::init()) return 1;
    spdlog::info(" Start application");

    asio::io_context io;
    auto work_guard = asio::make_work_guard(io);



    std::vector<std::shared_ptr<SerialTransport>> serialPorts;

    try{
        std::vector<PortConfig> ports = ConfigLoader::load("/home/ubuntu/CLionProjects/OpcSerialBridge/config/config.yaml");

        spdlog::info(" Ports number : {}", ports.size());

        for (auto& port : ports){
            spdlog::info(" port path : {}", port.port_path);
            spdlog::info(" port index : {}", port.index);
            spdlog::info(" devices number : {}", port.devices.size());
            for (auto& device : port.devices){
                spdlog::info(" device address : {}", device.address);
            }
            auto serialPort = std::make_shared<SerialTransport>(io, port.port_path, port.index);
            serialPorts.push_back(serialPort);
        }


    }catch (std::exception& e){
        spdlog::critical(" Exeption : {} ", e.what());
        return 1;
    }
    auto opcClient = std::make_shared<OpcTransport>(io, "opc.tcp://192.168.163.123:4840");

    auto bridge = std::make_shared<OpcSerialBridge>(io, opcClient);

    for (auto& serialPort : serialPorts){
        bridge->add_serial_port(serialPort);
    }
    bridge->run();


    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([bridge, &io, work_guard](auto, int) mutable {
        spdlog::info("Signal received. Stopping bridge...");
        bridge->stop();
        work_guard.reset();
        io.stop();
    });

    io.run();
    return 0;
}
