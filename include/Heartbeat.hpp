#pragma once

#include "OpcTransport.hpp"

#include <chrono>
#include <cstdint>
#include <string>

// Счётчик «мост жив» в узел keepAlive. ПЛК следит, что значение меняется,
// и останавливает оси, если оно замерло. Счётчик растёт всегда, в том числе без связи.
class Heartbeat {
public:
    Heartbeat(asio::io_context& io, OpcTransport& opc, const std::string& node, std::chrono::milliseconds period);
    Heartbeat(const Heartbeat&) = delete;
    Heartbeat& operator=(const Heartbeat&) = delete;

    void start();
    void stop();
    uint32_t counter() const { return counter_; }

private:
    void schedule();

    OpcTransport& opc_;
    opcua::NodeId node_;
    std::chrono::milliseconds period_;
    asio::steady_timer timer_;
    uint32_t counter_{0};
    bool running_{false};
};
