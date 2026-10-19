#pragma once

#include "Config.hpp"
#include "DeviceChannel.hpp"
#include "Heartbeat.hpp"
#include "OpcTransport.hpp"
#include "PortWorker.hpp"

#include <deque>
#include <memory>

class App {
public:
    explicit App(BridgeConfig config);
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();
    void request_stop();

    const std::deque<PortWorker>& ports() const { return ports_; }
    const std::deque<DeviceChannel>& devices() const { return devices_; }

private:
    void stop();
    void schedule_stats();
    void log_stats();

    asio::io_context io_;
    BridgeConfig config_;
    OpcTransport opc_;
    std::unique_ptr<Heartbeat> heartbeat_;
    std::deque<PortWorker> ports_;
    std::deque<DeviceChannel> devices_;
    asio::signal_set signals_{io_, SIGINT, SIGTERM};
    asio::steady_timer stats_timer_{io_};
    bool stopping_{false};
};
