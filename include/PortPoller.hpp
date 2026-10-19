#pragma once

#include "Config.hpp"
#include "DeviceChannel.hpp"
#include "PortPublisher.hpp"

#include <chrono>
#include <string>
#include <vector>

// Опрос плат одного порта кругами.
// Круг: всем платам, чья очередь, GET_STATUS разом в очередь порта; платы отвечают по очереди.
// Когда ответили (или не ответили) все — конец круга: статусы уходят в ПЛК одной пачкой.
// Если круг не закончился к следующему тику (много команд, молчащие платы), тик пропускается:
// пропущенные круги не догоняем, иначе очередь порта растёт.
class PortPoller {
public:
    struct Stats {
        uint64_t cycles{0};
        uint64_t overruns{0};                       // тиков, пропущенных из-за незаконченного круга
        std::chrono::milliseconds max_cycle{0};     // самый долгий круг с прошлой сводки
    };

    PortPoller(asio::io_context& io, PortPublisher& publisher, std::chrono::milliseconds period,
               PollSchedule schedule, bool flush_after_cycle, std::string name);
    PortPoller(const PortPoller&) = delete;
    PortPoller& operator=(const PortPoller&) = delete;

    void add(DeviceChannel& device);    // до start()
    void start();
    void stop();

    const std::string& name() const { return name_; }
    const Stats& stats() const { return stats_; }
    void reset_max_cycle() { stats_.max_cycle = {}; }

private:
    using Clock = std::chrono::steady_clock;

    void wait_until(Clock::time_point when);
    void tick();
    void cycle_finished();

    PortPublisher& publisher_;
    asio::steady_timer timer_;
    std::chrono::milliseconds period_;
    PollSchedule schedule_;
    bool flush_after_cycle_;
    std::string name_;
    std::vector<DeviceChannel*> devices_;

    bool running_{false};
    uint64_t cycle_{0};
    size_t waiting_{0};                 // опросов текущего круга без ответа
    Clock::time_point next_tick_{};
    Clock::time_point cycle_started_{};
    bool overrun_logged_{false};
    Stats stats_;
};
