
#ifndef SERIALDRIVER_OPCCLIENT_HPP
#define SERIALDRIVER_OPCCLIENT_HPP
#include <open62541pp/open62541pp.hpp>
#include <boost/asio.hpp>
#include <map>

namespace asio = boost::asio;
using DataChangeCallback = std::function<void(const opcua::Variant&)>;


class OpcTransport {
public:
    OpcTransport(asio::io_context& ioc, const std::string& ep);
    ~OpcTransport();

    void initialize();
    void stop();
    void run();
    void register_node(const opcua::NodeId& nodeId, DataChangeCallback callback);
    void write_to_node(const opcua::NodeId& nodeId, const opcua::Variant& value, std::function<void(void)> onComplete);
    void read_from_node(const opcua::NodeId& nodeId, std::function<void(const opcua::NodeId& nodeId, opcua::Variant)> onComplete);
private:

    void setup_subscriptions();

    asio::awaitable<void> reconnect();
    asio::awaitable<void> run_loop();
    void connect();

    std::string endpoint;
    std::string name{"OPC UA"};
    opcua::Client client;
    asio::io_context &io;
    asio::steady_timer loopTimer;
    asio::steady_timer reconnectTimer;
    bool keep_running{false};
    bool session_activated{false};

    std::map<opcua::NodeId, DataChangeCallback> node_callbacks;
};


#endif //SERIALDRIVER_OPCCLIENT_HPP
