#pragma once

#include "DeviceNodes.hpp"
#include "OpcTransport.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

// Запись статусов плат одного порта в ПЛК — пачками, а не каждая плата отдельно.
//
// Плата отдаёт сюда свой статус (publish) и идёт дальше. Сборщик хранит по одному, последнему,
// статусу на плату и отправляет всё накопленное (flush) двумя запросами:
//   1) Write: info.* всех плат пачки;
//   2) Write: infoReady = true тех плат, чьи значения записались.
// Когда отправлять: flush_interval > 0 — по своему таймеру; 0 — flush() зовёт PortPoller в конце круга опроса.
// Ответ на команду (urgent) уходит сразу, вместе со всем накопленным.
// Пока пачка пишется, следующая не уходит: статусы копятся, старый статус платы заменяется новым.
class PortPublisher {
public:
    // Вызывается один раз после первой попытки записать статус: ok — значения и infoReady в ПЛК.
    using Sent = std::function<void(bool ok)>;

    struct Stats {
        uint64_t batches{0};        // пачек (по 2 запроса Write)
        uint64_t statuses{0};       // статусов в них
        uint64_t failed{0};         // пачек, не дошедших целиком (нет связи, тайм-аут)
    };

    PortPublisher(asio::io_context& io, OpcTransport& opc, std::chrono::milliseconds flush_interval,
                  size_t max_nodes_per_request, std::string name);
    PortPublisher(const PortPublisher&) = delete;
    PortPublisher& operator=(const PortPublisher&) = delete;

    void start();
    void stop();

    // nodes должны жить, пока жив сборщик: App хранит платы в deque, адреса не меняются.
    void publish(const DeviceNodes& nodes, const proto::StatusData& status, bool urgent, Sent sent = {});
    void flush();       // отправить накопленное сейчас; если пачка ещё пишется — ничего не делает

    const std::string& name() const { return name_; }
    const Stats& stats() const { return stats_; }

private:
    struct Item {
        const DeviceNodes* nodes;
        proto::StatusData status;
        bool urgent{false};
        std::vector<Sent> sent;
    };

    void schedule_tick();
    void write_ready(std::vector<Item> batch);
    void batch_finished();
    void put_back(Item item);

    OpcTransport& opc_;
    asio::steady_timer timer_;
    std::chrono::milliseconds flush_interval_;
    size_t max_devices_per_batch_;
    std::string name_;

    std::vector<Item> pending_;     // по одной записи на плату, в порядке поступления
    bool in_flight_{false};
    bool left_behind_{false};       // в прошлую пачку влезли не все — отправить остаток сразу
    bool running_{false};
    Stats stats_;
};
