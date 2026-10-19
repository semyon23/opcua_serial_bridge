#include "../include/Heartbeat.hpp"

Heartbeat::Heartbeat(asio::io_context& io, OpcTransport& opc, const std::string& node,
                     std::chrono::milliseconds period)
    : opc_(opc), node_(opcua::NodeId::parse(node)), period_(period), timer_(io) {}

void Heartbeat::start() {
    running_ = true;
    schedule();
}

void Heartbeat::stop() {
    running_ = false;
    timer_.cancel();
}

void Heartbeat::schedule() {
    timer_.expires_after(period_);
    timer_.async_wait([this](boost::system::error_code ec) {
        if (ec || !running_) return;
        opc_.write_node(node_, opcua::Variant(counter_++), {});
        schedule();
    });
}
