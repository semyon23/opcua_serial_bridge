
#include "../include/OpcTransport.hpp"
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

OpcTransport::OpcTransport(asio::io_context &ioc, const std::string& ep)
        : io(ioc), loopTimer(ioc), reconnectTimer(ioc), endpoint(ep){

    initialize();
}

OpcTransport::~OpcTransport() {
    spdlog::info("[{}] Call Destructor", name);
    stop();
}

void OpcTransport::initialize() {
    client.onConnected([this]() {
        spdlog::info("[{}] Connected to {}", name, endpoint);
        // asio::post(io, [this]() {
        //
        //  });
    });

    client.onSessionClosed([this]() {
        spdlog::info("[{}] Session closed!", name);
        // asio::post(io, [this]() {
        //
        // });
    });

    client.onInactive([this]() {
        spdlog::info("[{}]Client Inactive {}", name, endpoint);
        // asio::post(io, [this]() {
        //
        // });

    });

    client.onSessionActivated([this]() {
        // asio::post(io, [this]() {
        //
        // });
            spdlog::info("[{}] Session Active. Setting up subscriptions", name);
                        session_activated = true;
                         setup_subscriptions();
    });

    client.onDisconnected([this] {
        spdlog::info("[{}] Disconnected!", name);
            session_activated = false;
        // asio::post(io, [this]() {
        //
        // });

    });

    client.config().setTimeout(1000);
    client.config().setSecurityMode(opcua::MessageSecurityMode::None);
}

void OpcTransport::connect() {
    spdlog::info("[{}] Attempting connection... {}", name, endpoint);

    try {
        spdlog::info("[{}] Initializing async connection to {}...", name, endpoint);
        client.connectAsync(endpoint);
    }
    catch (const std::exception& e) {
        spdlog::info("[{}] Critical connection init error: {}", name, e.what());
    }
}

void OpcTransport::stop() {
    keep_running = false;
    loopTimer.cancel();
    reconnectTimer.cancel();
    client.stop();
}

void OpcTransport::run() {
    keep_running = true;
    connect();
    asio::co_spawn(io, run_loop(), asio::detached);
}

asio::awaitable<void> OpcTransport::reconnect() {
    spdlog::info("[{}] Reconnection...", name);
    while (keep_running) {
        reconnectTimer.expires_after(std::chrono::seconds(2));

        boost::system::error_code ec;
        co_await reconnectTimer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        if (ec == asio::error::operation_aborted || !keep_running) {
            co_return;
        }

        try {
            connect();
            co_return;
        }
        catch (const std::exception& e) {
            spdlog::info("[{}] Error : {}", name, e.what());
        }
    }
}

asio::awaitable<void> OpcTransport::run_loop() {

    while (keep_running) {
        bool need_reconnect = false;
        try {
            client.runIterate(0);
        }
        catch (const opcua::BadStatus& e) {
            spdlog::info("[{}] ERROR: {}", name, e.what());
            need_reconnect = true;
        }

        if (need_reconnect) {
            co_await reconnect();
            continue;
        }

        loopTimer.expires_after(std::chrono::milliseconds(10));

        boost::system::error_code ec;
        co_await loopTimer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

        if (ec == asio::error::operation_aborted || !keep_running) {
            spdlog::info("[{}] Operation aborted", name);
            co_return;
        }
    }
}

void OpcTransport::read_from_node(const opcua::NodeId& nodeId, std::function<void(const opcua::NodeId& nodeId, opcua::Variant)> onComplete) {
    if (!session_activated) {
        spdlog::info("[{}] Session is inactive", name);
        return;
    }

    opcua::services::readValueAsync(
        client,
        nodeId,
        [this,nodeId,onComplete](opcua::Result<opcua::Variant>& result) {
            if (result.code().isGood()) {
                if (onComplete) onComplete(nodeId, result.value());
            } else {
                spdlog::info("[{}] ERROR read: {}", name, result.code().name());
            }
        });
}


void OpcTransport::write_to_node(const opcua::NodeId& nodeId, const opcua::Variant &value, std::function<void()> onComplete) {
    if (!session_activated) {
        spdlog::info("[{}] Session is inactive", name);
        return;
    }

    opcua::services::writeValueAsync(
           client,
           nodeId,
           value,
           [this, nodeId, onComplete](opcua::StatusCode status) {
               if (status.isGood()) {
                   if (onComplete) onComplete();
               } else {
                   spdlog::error("[{}] ERROR write Node ID :\"{}\" - {}", name, std::string(opcua::toString(nodeId)), status.name());

               }
           }
       );
}

void OpcTransport::register_node(const opcua::NodeId& nodeId, DataChangeCallback callback) {
    node_callbacks[nodeId] = std::move(callback);

}

void OpcTransport::setup_subscriptions() {
    if (node_callbacks.empty()) return;

    auto params = opcua::SubscriptionParameters{5, 100, 10, 0, 0};

    opcua::services::createSubscriptionAsync(
        client,
        params,
        true,
{},
        {},
        [this](opcua::CreateSubscriptionResponse& response) {
            auto mp =  opcua::MonitoringParametersEx({}, 1,{}, 10, false);
            auto subId = response.subscriptionId();
            spdlog::info("[{}]Created Subscription ID: {}", name, subId);

            for (const auto& [node, callback] : node_callbacks) {
                opcua::ReadValueId rvi{node, opcua::AttributeId::Value};
                opcua::services::createMonitoredItemDataChangeAsync(
                    client,
                    subId,
                    rvi,
                    opcua::MonitoringMode::Reporting,
                    mp,
                    [this, callback](opcua::IntegerId sId, opcua::IntegerId mId, const opcua::DataValue& dv) {
                        if (dv.status().isGood() && callback) {
                            callback(dv.value());
                        }
                    },
                    {},
                    [this](opcua::MonitoredItemCreateResult& result) {
                        if (result.statusCode().isGood()) {
                            spdlog::info("[{}] Successfully monitored  with monId: {}", name, result.monitoredItemId());
                        }
                    });
            }
        }
    );
}

