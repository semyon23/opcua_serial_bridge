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
        const auto port_name = "port " + std::to_string(pc.index);
        PortPublisher& publisher = publishers_.emplace_back(io_, opc_, pc.status_flush, config_.max_nodes_per_request,
                                                            port_name);
        PortPoller& poller = pollers_.emplace_back(io_, publisher, pc.poll_cycle, pc.poll_schedule,
                                                   pc.status_flush.count() == 0, port_name);
        for (const DeviceConfig& dc : pc.devices) {
            auto nodes = DeviceNodes::make(config_.device_node_pattern, pc.index, dc.index);
            auto name = port_name + " / device " + std::to_string(dc.address);
            poller.add(devices_.emplace_back(io_, opc_, port, publisher, std::move(nodes), dc, std::move(name)));
        }
    }
}

int App::run() {
    spdlog::info("[BRIDGE] {} port(s), {} device(s), PLC {}", ports_.size(), devices_.size(), config_.opc.endpoint);
    for (auto& device : devices_) device.start();   // сначала узлы cmdReady в подписку
    for (auto& port : ports_) port.start();
    for (auto& publisher : publishers_) publisher.start();
    for (auto& poller : pollers_) poller.start();
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
    for (auto& poller : pollers_) poller.stop();
    for (auto& device : devices_) device.stop();
    if (heartbeat_) heartbeat_->stop();
    for (auto& publisher : publishers_) publisher.stop();
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
    for (auto& poller : pollers_) {
        const auto& s = poller.stats();
        spdlog::info("[STATS] {} polling: {} cycles, longest {} ms, {} skipped (cycle longer than the period)",
                     poller.name(), s.cycles, s.max_cycle.count(), s.overruns);
        poller.reset_max_cycle();
    }
    for (const auto& publisher : publishers_) {
        const auto& s = publisher.stats();
        spdlog::info("[STATS] {} -> PLC: {} batches, {} statuses ({:.1f} per batch), {} failed", publisher.name(),
                     s.batches, s.statuses, s.batches ? double(s.statuses) / double(s.batches) : 0.0, s.failed);
    }
    size_t online = 0;
    for (const auto& device : devices_) online += device.online();
    spdlog::info("[STATS] devices online: {} of {}; OPC UA session {}", online, devices_.size(),
                 opc_.session_active() ? "active" : "inactive");
}
