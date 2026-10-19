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

enum class PollSchedule {
    Fixed,          // круги по сетке: 0, 100, 200 мс… — не зависят от ответов и команд
    AfterReply,     // следующий круг — через период после конца прошлого
};

// Периоды опроса платы округляются до целого числа кругов порта (круг = status_poll_ms порта).
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
    std::chrono::milliseconds poll_cycle{100};    // круг опроса порта = его status_poll_ms
    PollSchedule poll_schedule{PollSchedule::Fixed};
    std::chrono::milliseconds status_flush{0};    // 0 — пачка в ПЛК в конце круга опроса; > 0 — по таймеру
    std::vector<DeviceConfig> devices;
};

struct BridgeConfig {
    OpcSettings opc;
    std::string device_node_pattern;        // база узлов устройства: {port}, {device}
    std::string keep_alive_node;
    std::chrono::milliseconds keep_alive_period{1000};
    size_t max_nodes_per_request{100};      // лимит сервера на узлы в одном Write; больше — пачка делится
    std::vector<PortConfig> ports;
};

BridgeConfig load_config(const std::filesystem::path& path);
BridgeConfig parse_config(const std::string& yaml_text);         // то же из строки — для тестов
