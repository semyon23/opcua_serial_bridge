#pragma once

#include "OpcTransport.hpp"
#include "PortWorker.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct PollSettings {
    std::chrono::milliseconds status_poll{100};        // опрос GET_STATUS, пока плата отвечает
    std::chrono::milliseconds offline_poll{1000};      // опрос, когда плата не отвечает
    int offline_after{3};                              // тайм-аутов подряд — плата offline
    std::chrono::milliseconds status_refresh{1000};    // статус пишется в ПЛК и без изменений — раз в столько
};

struct DeviceConfig {
    uint8_t address{1};             // ID платы на шине
    uint16_t index{1};              // {device} в шаблоне узлов ПЛК
    PollSettings poll;
};

struct PortConfig {
    uint16_t index{1};              // {port} в шаблоне узлов ПЛК
    PortSettings serial;            // путь, скорость, тайм-аут ответа, переоткрытие
    std::vector<DeviceConfig> devices;
};

struct NodeOptions {
    std::string device_pattern;     // база узлов устройства: {port}, {device}
    bool deceleration{false};       // в ПЛК есть control.deceleration (иначе торможение = ускорению)
    bool command_result{false};     // мост пишет итог команды в cmdResult
    bool online{false};             // мост пишет info.online: отвечает ли плата
};

struct BridgeConfig {
    OpcSettings opc;
    NodeOptions nodes;
    std::string keep_alive_node;
    std::chrono::milliseconds keep_alive_period{1000};
    std::vector<PortConfig> ports;
};

BridgeConfig load_config(const std::filesystem::path& path);
BridgeConfig parse_config(const std::string& yaml_text);         // то же из строки — для тестов
