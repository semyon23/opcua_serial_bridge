
#ifndef OPCSERIALBRIDGE_SERIALTRANSPORT_HPP
#define OPCSERIALBRIDGE_SERIALTRANSPORT_HPP

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <queue>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <vector>

namespace asio = boost::asio;
using namespace asio::experimental::awaitable_operators;

struct SerialTransaction {
    std::vector<uint8_t> tx_data;
    std::size_t expected_rx_size{0};
    uint8_t device{0};
};

struct SerialResponse {
    uint8_t port_id;
    uint8_t device_id;
    std::vector<uint8_t> data;
};



class SerialTransport {
public:
    SerialTransport(asio::io_context& ioc, const std::string& name, uint8_t id);
    ~SerialTransport();
    void push_request(SerialTransaction& transaction);
    asio::awaitable<std::optional<SerialResponse>> get_response();
    void run();
    void stop();
    uint8_t get_port_id();

private:
    asio::awaitable<bool> send_message(const std::vector<uint8_t>& msg);
    asio::awaitable<std::optional<std::vector<uint8_t>>>  receive_response(std::chrono::milliseconds timeout, std::size_t expected_rx_size);
    asio::awaitable<void> run_queue_loop();

    std::string port_name;
    uint8_t port_id;
    asio::serial_port serial;
    asio::steady_timer timeout_timer;

    asio::experimental::concurrent_channel<void(boost::system::error_code, SerialResponse)> answer_channel;
    asio::experimental::concurrent_channel<void(boost::system::error_code, SerialTransaction)> request_channel;
};


#endif //OPCSERIALBRIDGE_SERIALTRANSPORT_HPP
