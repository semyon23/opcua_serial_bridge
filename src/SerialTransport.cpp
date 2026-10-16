#include "../include/SerialTransport.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <chrono>
#include <spdlog/fmt/bin_to_hex.h> // Оставляем для совместимости маппинга
#include <string_view>
#include <cstdint>
using namespace std::chrono_literals;

void log_bytes(std::string_view direction, std::string_view port_name, std::span<const uint8_t> data) {
    spdlog::info("[SERIAL] [{}] [{}] ({} bytes): {:X}",
                 port_name, direction, data.size(), spdlog::to_hex(data.begin(), data.end()));
}

SerialTransport::SerialTransport(asio::io_context &ioc, const std::string& name, uint8_t id)
    : port_name(name),
      port_id(id),
      serial(ioc),
      timeout_timer(ioc),
      request_channel(ioc, 50),
      answer_channel(ioc, 50)
{
    boost::system::error_code ec;
    serial.open(port_name, ec);

    if (ec) {
        spdlog::error(" Failed to open port {}: {}",port_name, ec.message());
        throw boost::system::system_error(ec, "SerialTransport failed to open port " + port_name);
    }

    serial.set_option(asio::serial_port_base::baud_rate(115200));
    serial.set_option(asio::serial_port_base::character_size(8));
    serial.set_option(asio::serial_port_base::flow_control(asio::serial_port_base::flow_control::none));
    serial.set_option(asio::serial_port_base::parity(asio::serial_port_base::parity::none));
    serial.set_option(asio::serial_port_base::stop_bits(asio::serial_port_base::stop_bits::one));

    spdlog::info("[SERIAL] - ID [{}]: {} successfully opened and configured!", port_id, port_name);
}

SerialTransport::~SerialTransport() {
    spdlog::info("[SERIAL] - [{}]: Stopping", port_name);
    stop();
}

void SerialTransport::run() {
    asio::co_spawn(serial.get_executor(), run_queue_loop(), asio::detached);
}

void SerialTransport::stop() {
    request_channel.close();
    answer_channel.close();
    timeout_timer.cancel();
    boost::system::error_code ec;
    serial.cancel(ec);
    serial.close(ec);
}

asio::awaitable<std::optional<SerialResponse>> SerialTransport::get_response() {
    boost::system::error_code ec;

    if (!answer_channel.is_open()) co_return std::nullopt;

    auto transaction = co_await answer_channel.async_receive(asio::redirect_error(asio::use_awaitable, ec));
    if (ec) co_return std::nullopt;
    co_return transaction;
}

void SerialTransport::push_request(SerialTransaction &transaction) {
    if (transaction.tx_data.empty()) return;
    request_channel.try_send(boost::system::error_code{}, transaction);
}

uint8_t SerialTransport::get_port_id() {
    return port_id;
}

asio::awaitable<bool> SerialTransport::send_message(const std::vector<uint8_t> &msg) {

    auto [ec, bytes_written] = co_await asio::async_write(
        serial,
        asio::buffer(msg),
        asio::as_tuple(asio::use_awaitable)
    );

    if (ec) {
        spdlog::error("[SERIAL] [{}] - Write Error {}", port_name, ec.message());
        co_return false;
    }
    co_return true;
}

asio::awaitable<std::optional<std::vector<uint8_t> > > SerialTransport::receive_response(std::chrono::milliseconds timeout, std::size_t expected_rx_size) {
    std::vector<uint8_t> local_buffer(expected_rx_size);

    auto [ec, bytes_read] = co_await asio::async_read(
        serial,
        asio::buffer(local_buffer),
        asio::as_tuple(asio::cancel_after(timeout, asio::use_awaitable))
    );

    if (ec) {
        spdlog::error("[SERIAL] [{}] - Read error: {}", port_name, ec.message());
        co_return std::nullopt;
    }
    co_return local_buffer;
}

asio::awaitable<void> SerialTransport::run_queue_loop() {
    while (true) {
        boost::system::error_code ec;

        SerialTransaction current_transaction = co_await request_channel.async_receive(
            asio::redirect_error(asio::use_awaitable, ec)
        );

        if (ec == asio::experimental::channel_errc::channel_cancelled) {
            co_return;
        }
        // tcflush(serial.lowest_layer().native_handle(), TCIFLUSH);
        tcflush(serial.lowest_layer().native_handle(), TCIOFLUSH);

       // spdlog::warn("[SERIAL] [{}] - Send message: device[{}]", port_name, current_transaction.device);
       // log_bytes("TX", port_name, current_transaction.tx_data);
        bool write_success = co_await send_message(current_transaction.tx_data);
        if (!write_success) {
            continue;
        }

        auto response = co_await receive_response(std::chrono::milliseconds(30), current_transaction.expected_rx_size);
        //spdlog::warn("[SERIAL] [{}] - Response has been received:  device[{}]", port_name, current_transaction.device);
        if (!response.has_value()) {
            spdlog::error("[SERIAL] [{}] - Read error: device {}", port_name, current_transaction.device);
            continue;
        }
       // log_bytes("RX", port_name, response.value());

        SerialResponse res{port_id, current_transaction.device, response.value()};
        answer_channel.try_send(boost::system::error_code{}, res);
    }
}



// try {
//     timeout_timer.expires_after(timeout);
//
//     auto result = co_await (
//         asio::async_read(serial, asio::buffer(local_buffer), asio::use_awaitable)
//         ||
//         timeout_timer.async_wait(asio::use_awaitable)
//     );
//
//     if (result.index() == 1) {
//         serial.cancel();
//         co_return std::nullopt;
//     }
//
//     timeout_timer.cancel();
//     co_return local_buffer;
//
// } catch (const boost::system::system_error& e) {
//     if (e.code() == asio::error::operation_aborted) {
//         tcflush(serial.lowest_layer().native_handle(), TCIOFLUSH);
//     } else {
//         spdlog::error("[SERIAL] [{}] - Read error: {}", port_name, e.what());
//         boost::system::error_code ec;
//         serial.cancel(ec);
//         tcflush(serial.lowest_layer().native_handle(), TCIOFLUSH);
//     }
//     co_return std::nullopt;
//}