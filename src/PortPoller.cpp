#include "../include/PortPoller.hpp"

#include <spdlog/spdlog.h>

PortPoller::PortPoller(asio::io_context& io, PortPublisher& publisher, std::chrono::milliseconds period,
                       PollSchedule schedule, bool flush_after_cycle, std::string name)
    : publisher_(publisher),
      timer_(io),
      period_(period),
      schedule_(schedule),
      flush_after_cycle_(flush_after_cycle),
      name_(std::move(name)) {}

void PortPoller::add(DeviceChannel& device) {
    devices_.push_back(&device);
}

void PortPoller::start() {
    running_ = true;
    // первый круг — через период: к этому времени порт уже открыт
    next_tick_ = Clock::now() + period_;
    wait_until(next_tick_);
}

void PortPoller::stop() {
    running_ = false;
    timer_.cancel();
}

void PortPoller::wait_until(Clock::time_point when) {
    timer_.expires_at(when);
    timer_.async_wait([this](boost::system::error_code ec) {
        if (!ec && running_) tick();
    });
}

void PortPoller::tick() {
    if (schedule_ == PollSchedule::Fixed) {
        // следующий тик — от сетки, а не от «сейчас»; отстали больше чем на период — сетку сдвигаем
        next_tick_ += period_;
        if (next_tick_ < Clock::now()) next_tick_ = Clock::now() + period_;
        wait_until(next_tick_);
    }

    if (waiting_ > 0) {                 // прошлый круг ещё идёт — этот пропускаем
        ++stats_.overruns;
        if (!overrun_logged_) {
            spdlog::warn("[{}] poll cycle longer than {} ms: {} polls still waiting; the next cycle is skipped",
                         name_, period_.count(), waiting_);
            overrun_logged_ = true;
        }
        return;
    }

    ++cycle_;
    cycle_started_ = Clock::now();
    std::vector<DeviceChannel*> due;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i]->poll_due(cycle_ + i, period_)) due.push_back(devices_[i]);
    }
    waiting_ = due.size();
    if (due.empty()) {
        cycle_finished();
        return;
    }
    for (DeviceChannel* device : due) {
        device->poll([this] {
            if (running_ && --waiting_ == 0) cycle_finished();
        });
    }
}

void PortPoller::cycle_finished() {
    ++stats_.cycles;
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - cycle_started_);
    stats_.max_cycle = std::max(stats_.max_cycle, took);
    if (took <= period_) overrun_logged_ = false;

    if (flush_after_cycle_) publisher_.flush();
    if (schedule_ == PollSchedule::AfterReply) wait_until(Clock::now() + period_);
}
