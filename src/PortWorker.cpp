#include "../include/PortWorker.hpp"

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>

#if defined(__unix__) || defined(__APPLE__)
#include <termios.h>
#endif

using namespace boost::asio::experimental::awaitable_operators;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kTimeoutLogInterval = std::chrono::seconds(5);

}  // namespace

// ------------------------------------------------------------------ PortWorker

PortWorker::PortWorker(asio::io_context& io, PortSettings settings)
    : io_(io), settings_(std::move(settings)), serial_(io), wake_(io), read_timer_(io) {}

PortWorker::~PortWorker() {
    stop();
}

void PortWorker::start() {
    if (running_) return;
    running_ = true;
    asio::co_spawn(io_, loop(), [path = settings_.path](std::exception_ptr e) {
        if (!e) return;
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            spdlog::critical("[SERIAL {}] worker stopped: {}", path, ex.what());
        }
    });
}

void PortWorker::stop() {
    if (!running_) return;
    running_ = false;
    wake_.cancel();
    read_timer_.cancel();
    boost::system::error_code ec;
    serial_.close(ec);                   // текущее чтение или запись завершится с operation_aborted
    fail_queued();
}

void PortWorker::submit(uint8_t address, uint8_t cmd, std::vector<uint8_t> payload, Priority priority,
                        ReplyHandler handler) {
    auto& queue = priority == Priority::Command ? commands_ : polls_;
    if (!running_ || !serial_.is_open() || queue.size() >= settings_.max_queue) {
        ++stats_.rejected;
        complete(std::move(handler), std::nullopt);
        return;
    }
    queue.push_back(Request{address, cmd, std::move(payload), priority, std::move(handler)});
    wake_.cancel();                      // если цикл спит без работы — разбудить
}

bool PortWorker::poll_pending(uint8_t address) const {
    if (in_flight_ && in_flight_->first == address && in_flight_->second == Priority::Poll) return true;
    return std::any_of(polls_.begin(), polls_.end(), [address](const Request& r) { return r.address == address; });
}

asio::awaitable<void> PortWorker::loop() {
    while (running_) {
        if (!serial_.is_open()) {
            if (!co_await open_port()) continue;          // не открылся — внутри уже подождали
        }

        std::deque<Request>* queue = !commands_.empty() ? &commands_ : !polls_.empty() ? &polls_ : nullptr;
        if (queue == nullptr) {
            wake_.expires_at(Clock::time_point::max());
            co_await wake_.async_wait(asio::as_tuple(asio::use_awaitable));
            continue;
        }

        Request req = std::move(queue->front());
        queue->pop_front();
        in_flight_ = std::make_pair(req.address, req.priority);
        auto reply = co_await exchange(req);
        in_flight_.reset();
        complete(std::move(req.handler), std::move(reply));
    }
}

asio::awaitable<bool> PortWorker::open_port() {
    boost::system::error_code ec;
    serial_.open(settings_.path, ec);
    if (!ec) serial_.set_option(asio::serial_port_base::baud_rate(settings_.baud_rate), ec);
    if (!ec) serial_.set_option(asio::serial_port_base::character_size(8), ec);
    if (!ec) serial_.set_option(asio::serial_port_base::parity(settings_.parity), ec);
    if (!ec) serial_.set_option(asio::serial_port_base::stop_bits(settings_.stop_bits), ec);
    if (!ec) serial_.set_option(asio::serial_port_base::flow_control(asio::serial_port_base::flow_control::none), ec);

    if (ec) {
        boost::system::error_code ignored;
        serial_.close(ignored);
        if (!open_failed_logged_) {          // не повторять одно и то же каждые 2 секунды
            spdlog::error("[SERIAL {}] cannot open: {}; retrying every {} ms", settings_.path, ec.message(),
                          settings_.reopen_delay.count());
            open_failed_logged_ = true;
        }
        fail_queued();
        wake_.expires_after(settings_.reopen_delay);
        co_await wake_.async_wait(asio::as_tuple(asio::use_awaitable));
        co_return false;
    }

    ++stats_.reopens;
    open_failed_logged_ = false;
    parser_.reset();
    rx_pos_ = rx_len_ = 0;
    spdlog::info("[SERIAL {}] opened, {} baud", settings_.path, settings_.baud_rate);
    co_return true;
}

asio::awaitable<std::optional<Reply>> PortWorker::exchange(const Request& req) {
    ++stats_.requests;
#if defined(__unix__) || defined(__APPLE__)
    ::tcflush(serial_.native_handle(), TCIFLUSH);         // хвосты прошлых ответов — в мусор
#endif
    parser_.reset();
    rx_pos_ = rx_len_ = 0;

    const auto frame = proto::encode_frame(req.address, req.cmd, req.payload);
    auto [write_ec, written] = co_await asio::async_write(serial_, asio::buffer(frame),
                                                          asio::as_tuple(asio::use_awaitable));
    if (write_ec) {
        if (write_ec != asio::error::operation_aborted) close_port("write: " + write_ec.message());
        co_return std::nullopt;
    }

    const auto deadline = Clock::now() + settings_.response_timeout;
    while (true) {
        auto reply = co_await read_frame(deadline);
        if (!reply) {
            if (running_ && serial_.is_open()) {
                ++stats_.timeouts;
                log_timeout(req.address, req.cmd);
            }
            co_return std::nullopt;
        }
        if (reply->raw == frame) continue;                  // эхо своего запроса (адаптер RS-485 с эхом)
        if (reply->address == req.address && reply->cmd == req.cmd) {
            ++stats_.replies;
            co_return Reply{reply->cmd, std::move(reply->payload)};
        }
        // опоздавший ответ другой платы или чужой кадр на шине — не наш, ждём дальше
        ++stats_.foreign_frames;
        spdlog::debug("[SERIAL {}] skip frame from {} cmd {} while waiting for {} cmd {}", settings_.path,
                      reply->address, reply->cmd, req.address, req.cmd);
    }
}

asio::awaitable<std::optional<proto::Frame>> PortWorker::read_frame(Clock::time_point deadline) {
    while (true) {
        while (rx_pos_ < rx_len_) {                         // сначала — то, что уже прочитано
            if (auto frame = parser_.feed(rx_[rx_pos_++])) co_return frame;
        }
        if (Clock::now() >= deadline) co_return std::nullopt;

        read_timer_.expires_at(deadline);
        auto result = co_await (serial_.async_read_some(asio::buffer(rx_), asio::as_tuple(asio::use_awaitable)) ||
                                read_timer_.async_wait(asio::as_tuple(asio::use_awaitable)));
        if (result.index() == 1) co_return std::nullopt;   // тайм-аут
        auto [ec, n] = std::get<0>(result);
        if (ec) {
            if (ec != asio::error::operation_aborted) close_port("read: " + ec.message());
            co_return std::nullopt;
        }
        rx_pos_ = 0;
        rx_len_ = n;
    }
}

void PortWorker::close_port(std::string_view reason) {
    spdlog::warn("[SERIAL {}] closed ({}); reopening in {} ms", settings_.path, reason,
                 settings_.reopen_delay.count());
    boost::system::error_code ec;
    serial_.close(ec);
    fail_queued();
}

void PortWorker::complete(ReplyHandler handler, std::optional<Reply> reply) {
    if (!handler) return;
    // всегда через очередь io_context: обработчик может сразу вызвать submit() — без вложенных вызовов
    asio::post(io_, [handler = std::move(handler), reply = std::move(reply)]() mutable {
        handler(std::move(reply));
    });
}

void PortWorker::fail_queued() {
    for (auto* queue : {&commands_, &polls_}) {
        while (!queue->empty()) {
            ++stats_.rejected;
            complete(std::move(queue->front().handler), std::nullopt);
            queue->pop_front();
        }
    }
}

void PortWorker::log_timeout(uint8_t address, uint8_t cmd) {
    auto& entry = timeout_log_[address];
    const auto now = Clock::now();
    if (now - entry.last < kTimeoutLogInterval) {
        ++entry.suppressed;
        return;
    }
    if (entry.suppressed > 0) {
        spdlog::warn("[SERIAL {}] no reply from device {} (cmd {}), {} more since last message", settings_.path,
                     address, cmd, entry.suppressed);
    } else {
        spdlog::warn("[SERIAL {}] no reply from device {} (cmd {})", settings_.path, address, cmd);
    }
    entry.last = now;
    entry.suppressed = 0;
}
