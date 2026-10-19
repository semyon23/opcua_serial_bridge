#include "../include/App.hpp"

#include <spdlog/spdlog.h>

namespace {
constexpr auto kStatsInterval = std::chrono::seconds(60);
}

App::App(BridgeConfig config) : config_(std::move(config)), opc_(io_, config_.opc) {
    if (!config_.keep_alive_node.empty()) {
        heartbeat_ = std::make_unique<Heartbeat>(io_, opc_, config_.keep_alive_node, config_.keep_alive_period);
    }
    for (const PortConfig& pc : config_.ports) {
        PortWorker& port = ports_.emplace_back(io_, pc.serial);
        for (const DeviceConfig& dc : pc.devices) {
            auto nodes = DeviceNodes::make(config_.nodes, pc.index, dc.index);
            auto name = "port " + std::to_string(pc.index) + " / device " + std::to_string(dc.address);
            devices_.emplace_back(io_, opc_, port, std::move(nodes), dc, config_.nodes, std::move(name));
        }
    }
}

int App::run() {
    spdlog::info("[BRIDGE] {} port(s), {} device(s), PLC {}", ports_.size(), devices_.size(), config_.opc.endpoint);
    for (auto& device : devices_) device.start();   // сначала узлы cmdReady в подписку
    for (auto& port : ports_) port.start();
    if (heartbeat_) heartbeat_->start();
    opc_.run();                                       // соединение: подписка сразу со всеми узлами

    signals_.async_wait([this](boost::system::error_code ec, int signal) {
        if (ec) return;
        spdlog::info("[BRIDGE] signal {} — stopping", signal);
        stop();
    });
    schedule_stats();
    io_.run();
    spdlog::info("[BRIDGE] stopped");
    return 0;
}

void App::request_stop() {
    asio::post(io_, [this] { stop(); });
}

void App::stop() {
    if (stopping_) return;
    stopping_ = true;
    // обратный порядок: сначала те, кто шлёт запросы, потом транспорты
    for (auto& device : devices_) device.stop();
    if (heartbeat_) heartbeat_->stop();
    for (auto& port : ports_) port.stop();
    opc_.stop();
    signals_.cancel();
    stats_timer_.cancel();
    io_.stop();
}

void App::schedule_stats() {
    stats_timer_.expires_after(kStatsInterval);
    stats_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;
        log_stats();
        schedule_stats();
    });
}

void App::log_stats() {
    for (const auto& port : ports_) {
        const auto& s = port.stats();
        spdlog::info("[STATS] {}: {} requests, {} replies, {} timeouts, {} foreign frames, {} bad CRC, {} rejected",
                     port.path(), s.requests, s.replies, s.timeouts, s.foreign_frames, port.crc_errors(), s.rejected);
    }
    size_t online = 0;
    for (const auto& device : devices_) online += device.online();
    spdlog::info("[STATS] devices online: {} of {}; OPC UA session {}", online, devices_.size(),
                 opc_.session_active() ? "active" : "inactive");
}
