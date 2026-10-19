#pragma once

#include "protocol.hpp"

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asio = boost::asio;

struct PortSettings {
    std::string path;                                   // /dev/ttyUSB0, /tmp/ttySim0
    unsigned baud_rate{115200};
    asio::serial_port_base::parity::type parity{asio::serial_port_base::parity::none};
    asio::serial_port_base::stop_bits::type stop_bits{asio::serial_port_base::stop_bits::one};
    std::chrono::milliseconds response_timeout{50};     // ответ платы — от конца записи запроса
    std::chrono::milliseconds reopen_delay{2000};       // пауза перед повторным открытием порта
    size_t max_queue{64};                                // на каждую из двух очередей
};

enum class Priority { Command, Poll };   // команды ПЛК уходят раньше опросов статуса

struct Reply {
    uint8_t cmd{0};
    std::vector<uint8_t> payload;
};

// nullopt — тайм-аут, порт закрыт, очередь переполнена или остановка.
// Обработчик вызывается всегда, ровно один раз и никогда — изнутри submit().
using ReplyHandler = std::function<void(std::optional<Reply>)>;

// Один последовательный порт с платами. Обмены — строго по одному: запрос, ответ, следующий.
// Ответ отдаётся только если в кадре тот же адрес и та же команда, что в запросе:
// опоздавший ответ другой платы пропускается.
// Всё — в потоке io_context. Объект живёт, пока работает io_context (им владеет App).
class PortWorker {
public:
    struct Stats {
        uint64_t requests{0};
        uint64_t replies{0};
        uint64_t timeouts{0};
        uint64_t foreign_frames{0};      // кадры не от того, кого спрашивали
        uint64_t rejected{0};            // порт закрыт или очередь переполнена
        uint64_t reopens{0};
    };

    PortWorker(asio::io_context& io, PortSettings settings);
    ~PortWorker();
    PortWorker(const PortWorker&) = delete;
    PortWorker& operator=(const PortWorker&) = delete;

    void start();
    void stop();

    void submit(uint8_t address, uint8_t cmd, std::vector<uint8_t> payload, Priority priority,
                ReplyHandler handler);
    bool poll_pending(uint8_t address) const;    // опрос этого устройства уже стоит в очереди
    bool is_open() const { return serial_.is_open(); }
    const Stats& stats() const { return stats_; }
    uint64_t crc_errors() const { return parser_.crc_errors(); }
    const std::string& path() const { return settings_.path; }

private:
    struct Request {
        uint8_t address;
        uint8_t cmd;
        std::vector<uint8_t> payload;
        Priority priority;
        ReplyHandler handler;
    };

    asio::awaitable<void> loop();
    asio::awaitable<bool> open_port();
    asio::awaitable<std::optional<Reply>> exchange(const Request& req);
    asio::awaitable<std::optional<proto::Frame>> read_frame(std::chrono::steady_clock::time_point deadline);
    void close_port(std::string_view reason);
    void complete(ReplyHandler handler, std::optional<Reply> reply);
    void fail_queued();
    void log_timeout(uint8_t address, uint8_t cmd);

    asio::io_context& io_;
    PortSettings settings_;
    asio::serial_port serial_;
    asio::steady_timer wake_;            // спит, пока нет запросов; submit() его будит
    asio::steady_timer read_timer_;
    std::deque<Request> commands_;
    std::deque<Request> polls_;
    proto::FrameParser parser_;
    std::array<uint8_t, 256> rx_{};
    size_t rx_pos_{0};                   // необработанный хвост последнего чтения
    size_t rx_len_{0};
    std::optional<std::pair<uint8_t, Priority>> in_flight_;   // обмен, который сейчас в линии
    bool running_{false};
    bool open_failed_logged_{false};
    Stats stats_{};

    struct TimeoutLog {
        std::chrono::steady_clock::time_point last;
        uint64_t suppressed{0};
    };
    std::map<uint8_t, TimeoutLog> timeout_log_;   // не чаще раза в 5 с на адрес
};
