
#ifndef FLEETMANAGERSYSTEM_CONFIGLOADER_HPP
#define FLEETMANAGERSYSTEM_CONFIGLOADER_HPP
#pragma once

#include <yaml-cpp/yaml.h>
#include "serial_config.hpp"

class ConfigLoader {
public:
    static std::vector<PortConfig> load(const std::string& file_path) {
        YAML::Node config;
        try {
            config = YAML::LoadFile(file_path);
        } catch (const std::exception& e) {
            throw std::runtime_error("Failed to load YAML file: " + std::string(e.what()));
        }

        SerialSettings global_settings;

        if (config["serial_parameters"]) {
            auto global_node = config["serial_parameters"];

            global_settings.baud_rate      = global_node["baud_rate"].as<unsigned int>(115200);
            global_settings.character_size = global_node["character_size"].as<unsigned int>(8);
            global_settings.parity         = global_node["parity"].as<std::string>("none");
            global_settings.stop_bits      = global_node["stop_bits"].as<std::string>("one");
            global_settings.flow_control   = global_node["flow_control"].as<std::string>("none");
        }

        std::vector<PortConfig> ports;

        if (config["serial_ports"]) {
            for (const auto& port_node : config["serial_ports"]) {
                PortConfig port;
                port.port_path = port_node["port"].as<std::string>();
                port.index = port_node["index"].as<unsigned int>();
                port.serial_settings = global_settings;

                if (port_node["devices"]) {
                    for (const auto& dev_node : port_node["devices"]) {
                        DeviceConfig dev;
                        dev.address = dev_node["address"].as<int>();
                        port.devices.push_back(dev);
                    }
                }

                ports.push_back(port);
            }
        }else{
            throw std::runtime_error("Configuration error: 'serial_ports' not found");
        }

        return ports;
    }
};

#endif //FLEETMANAGERSYSTEM_CONFIGLOADER_HPP
