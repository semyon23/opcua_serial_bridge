#include "../include/OpcTransport.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

std::string node_name(const opcua::NodeId& id) {
    return std::string(opcua::toString(id));
}

// Исключение из колбэка open62541pp ловит и перебрасывает из runIterate(),
// а оттуда — из корутины цикла: процесс падает. Колбэки пользователя — только через это.
template <typename Fn, typename... Args>
void call_user(std::string_view where, const Fn& fn, Args&&... args) {
    if (!fn) return;
    try {
        fn(std::forward<Args>(args)...);
    } catch (const std::exception& e) {
        spdlog::error("[OPC UA] Exception in {} callback: {}", where, e.what());
    }
}

}  // namespace

OpcTransport::OpcTransport(asio::io_context& ioc, OpcSettings settings)
    : settings_(std::move(settings)),
      io_(ioc),
      loop_timer_(ioc),
      reconnect_timer_(ioc),
      subscription_timer_(ioc) {
    if (settings_.reconnect_backoff.empty()) settings_.reconnect_backoff.push_back(std::chrono::seconds(2));
    configure_client();
}

OpcTransport::OpcTransport(asio::io_context& ioc, const std::string& endpoint)
    : OpcTransport(ioc, OpcSettings{.endpoint = endpoint}) {}

OpcTransport::~OpcTransport() {
    stop();
    *alive_ = false;          // колбэки, которые ещё придут при закрытии client_, ничего не делают
}

void OpcTransport::configure_client() {
    auto& config = client_.config();
    config.setTimeout(settings_.request_timeout_ms);
    config.setSecurityMode(settings_.security_mode);
    if (!settings_.username.empty()) {
        // В режиме None пароль идёт открытым текстом — сервер может отказать (BadIdentityTokenRejected)
        config.setUserIdentityToken(opcua::UserNameIdentityToken(settings_.username, settings_.password));
    }

    // Колбэки состояния приходят изнутри runIterate(); работу с клиентом откладываем
    // до выхода из него через post, чтобы не вызывать службы рекурсивно.
    client_.onConnected([this] {
        spdlog::info("[{}] Connected to {}", name_, settings_.endpoint);
    });
    client_.onSessionActivated([this, alive = std::weak_ptr<bool>(alive_)] {
        asio::post(io_, [this, alive] {
            if (auto a = alive.lock(); a && *a) on_session_activated();
        });
    });
    client_.onSessionClosed([this] {
        if (!*alive_) return;
        spdlog::info("[{}] Session closed", name_);
        session_active_ = false;
    });
    // Не повод переподключаться: срабатывает и при подключении, когда закрывается канал FindServers.
    // Потерянное соединение open62541 восстанавливает сама внутри runIterate(), а если не смогла —
    // runIterate() бросает исключение, и тогда переподключаемся мы (run_loop).
    client_.onDisconnected([this] {
        if (!*alive_) return;
        session_active_ = false;
    });
    client_.onInactive([this] {
        if (!*alive_) return;
        spdlog::warn("[{}] Server does not answer (inactive)", name_);
    });
}

// ------------------------------------------------------------------ соединение

void OpcTransport::run() {
    if (keep_running_) return;
    keep_running_ = true;
    reconnect_attempt_ = 0;
    connect();
    asio::co_spawn(io_, run_loop(), asio::detached);
}

void OpcTransport::stop() {
    if (!keep_running_) return;
    keep_running_ = false;
    session_active_ = false;
    reconnect_pending_ = false;
    loop_timer_.cancel();
    reconnect_timer_.cancel();
    subscription_timer_.cancel();
    try {
        client_.disconnect();       // CloseSession: ждёт ответа не дольше request_timeout_ms
    } catch (const std::exception& e) {
        spdlog::warn("[{}] Disconnect: {}", name_, e.what());
    }
}

bool OpcTransport::session_active() const {
    return session_active_;
}

std::chrono::milliseconds OpcTransport::change_detection_time() const {
    const double interval = std::max({revised_publishing_ms_, revised_sampling_ms_, settings_.publishing_interval_ms});
    return std::chrono::milliseconds(static_cast<long long>(std::ceil(interval)));
}

void OpcTransport::connect() {
    reconnect_pending_ = false;
    spdlog::info("[{}] Connecting to {} ...", name_, settings_.endpoint);
    try {
        client_.connectAsync(settings_.endpoint);
    } catch (const std::exception& e) {
        on_connection_lost(e.what());
    }
}

void OpcTransport::on_session_activated() {
    if (!keep_running_) return;
    session_active_ = true;
    reconnect_attempt_ = 0;
    reconnect_pending_ = false;
    reconnect_timer_.cancel();
    spdlog::info("[{}] Session active", name_);
    subscription_pending_ = false;
    create_subscription();
}

void OpcTransport::on_connection_lost(std::string_view reason) {
    if (!keep_running_) return;
    session_active_ = false;
    subscription_pending_ = false;
    ++subscription_generation_;          // ответы на запросы старой подписки больше не нужны
    subscription_timer_.cancel();
    if (reconnect_pending_) return;      // переподключение уже запланировано — не спамим журнал
    spdlog::warn("[{}] Connection lost: {}", name_, reason);
    schedule_reconnect();
}

void OpcTransport::schedule_reconnect() {
    const auto& backoff = settings_.reconnect_backoff;
    auto delay = backoff[std::min(reconnect_attempt_, backoff.size() - 1)];
    ++reconnect_attempt_;
    reconnect_pending_ = true;
    spdlog::info("[{}] Reconnect in {} ms", name_, delay.count());
    reconnect_timer_.expires_after(delay);
    reconnect_timer_.async_wait([this, alive = std::weak_ptr<bool>(alive_)](boost::system::error_code ec) {
        auto a = alive.lock();
        if (ec || !a || !*a || !keep_running_) return;
        connect();
    });
}

asio::awaitable<void> OpcTransport::run_loop() {
    while (keep_running_) {
        const auto started = std::chrono::steady_clock::now();
        try {
            client_.runIterate(0);
        } catch (const opcua::BadStatus& e) {
            on_connection_lost(e.what());
        } catch (const std::exception& e) {
            // сюда попадают исключения из колбэков open62541pp — соединение тут ни при чём
            spdlog::error("[{}] runIterate: {}", name_, e.what());
        }
        const auto spent = std::chrono::steady_clock::now() - started;
        if (spent > settings_.slow_iterate_warning) {
            spdlog::warn("[{}] runIterate took {} ms", name_,
                         std::chrono::duration_cast<std::chrono::milliseconds>(spent).count());
        }

        loop_timer_.expires_after(settings_.loop_interval);
        auto [ec] = co_await loop_timer_.async_wait(asio::as_tuple(asio::use_awaitable));
        if (ec || !keep_running_) co_return;
    }
}

// ------------------------------------------------------------------ подписка

void OpcTransport::register_node(const opcua::NodeId& node_id, DataChangeCallback callback) {
    node_callbacks_[node_id] = std::move(callback);
    if (session_active_) {
        subscription_pending_ = false;
        create_subscription();       // пересоздаём со всеми узлами
    }
}

void OpcTransport::create_subscription() {
    if (!session_active_ || node_callbacks_.empty() || subscription_pending_) return;
    subscription_pending_ = true;
    const uint64_t generation = ++subscription_generation_;

    try {
        // старая подписка (с прошлой сессии или неудачная) — удалить, иначе уведомления пойдут дважды
        if (subscription_id_) {
            const auto old_id = *subscription_id_;
            subscription_id_.reset();
            opcua::services::deleteSubscriptionAsync(client_, old_id, [](opcua::StatusCode) {});
        }

        opcua::SubscriptionParameters params{};
        params.publishingInterval = settings_.publishing_interval_ms;
        params.lifetimeCount = settings_.lifetime_count;
        params.maxKeepAliveCount = settings_.max_keep_alive_count;

        auto alive = std::weak_ptr<bool>(alive_);
        opcua::services::createSubscriptionAsync(
            client_, params, true,
            // сервер сообщил, что подписки больше нет (например, BadTimeout)
            [this, alive](opcua::IntegerId sub_id, opcua::StatusChangeNotification& notification) {
                auto a = alive.lock();
                if (!a || !*a) return;
                spdlog::warn("[{}] Subscription {} status: {}", name_, sub_id, notification.status().name());
                if (subscription_id_ == sub_id) {
                    subscription_id_.reset();
                    schedule_subscription_retry(std::chrono::milliseconds(500));
                }
            },
            // подписка удалена — не нами: создаём заново
            [this, alive](opcua::IntegerId sub_id) {
                auto a = alive.lock();
                if (!a || !*a) return;
                if (subscription_id_ == sub_id && keep_running_ && session_active_) {
                    spdlog::warn("[{}] Subscription {} deleted, recreating", name_, sub_id);
                    subscription_id_.reset();
                    schedule_subscription_retry(std::chrono::milliseconds(500));
                }
            },
            [this, alive, generation](opcua::CreateSubscriptionResponse& response) {
                auto a = alive.lock();
                if (!a || !*a || generation != subscription_generation_) return;
                on_subscription_created(response);
            });
    } catch (const std::exception& e) {
        spdlog::error("[{}] Create subscription: {}", name_, e.what());
        subscription_pending_ = false;
        schedule_subscription_retry(std::chrono::seconds(1));
    }
}

void OpcTransport::on_subscription_created(opcua::CreateSubscriptionResponse& response) {
    subscription_pending_ = false;
    const auto status = response.responseHeader().serviceResult();
    if (status.isBad()) {
        spdlog::error("[{}] Create subscription failed: {}", name_, status.name());
        schedule_subscription_retry(std::chrono::seconds(1));
        return;
    }
    subscription_id_ = response.subscriptionId();
    revised_publishing_ms_ = response.revisedPublishingInterval();
    revised_sampling_ms_ = 0;
    spdlog::info("[{}] Subscription {}: publishing {} ms, lifetime {} cycles, keep-alive {} cycles",
                 name_, response.subscriptionId(), response.revisedPublishingInterval(),
                 response.revisedLifetimeCount(), response.revisedMaxKeepAliveCount());

    opcua::MonitoringParametersEx monitoring{};
    monitoring.timestamps = opcua::TimestampsToReturn::Neither;
    monitoring.samplingInterval = settings_.sampling_interval_ms;
    monitoring.queueSize = settings_.queue_size;
    monitoring.discardOldest = true;

    const auto sub_id = response.subscriptionId();
    const uint64_t generation = subscription_generation_;
    auto alive = std::weak_ptr<bool>(alive_);
    for (const auto& [node, callback] : node_callbacks_) {
        try {
            opcua::services::createMonitoredItemDataChangeAsync(
                client_, sub_id, opcua::ReadValueId(node, opcua::AttributeId::Value),
                opcua::MonitoringMode::Reporting, monitoring,
                [this, alive, node, callback](opcua::IntegerId, opcua::IntegerId, const opcua::DataValue& dv) {
                    auto a = alive.lock();
                    if (!a || !*a) return;
                    if (!dv.status().isGood()) {
                        spdlog::warn("[{}] {}: {}", name_, node_name(node), dv.status().name());
                        return;
                    }
                    call_user("data change", callback, dv.value());
                },
                {},
                [this, alive, node, generation](opcua::MonitoredItemCreateResult& result) {
                    auto a = alive.lock();
                    if (!a || !*a || generation != subscription_generation_) return;
                    spdlog::debug("[{}] Monitored {} as mon {}: {}, sampling {} ms", name_, node_name(node),
                                  result.monitoredItemId(), result.statusCode().name(),
                                  result.revisedSamplingInterval());
                    revised_sampling_ms_ = std::max(revised_sampling_ms_, result.revisedSamplingInterval());
                    if (result.statusCode().isBad()) {
                        // например, узла нет в ПЛК: без него команды с этого устройства не дойдут
                        spdlog::error("[{}] Monitor {} failed: {}", name_, node_name(node),
                                      result.statusCode().name());
                        schedule_subscription_retry(std::chrono::seconds(5));
                    }
                });
        } catch (const std::exception& e) {
            spdlog::error("[{}] Monitor {}: {}", name_, node_name(node), e.what());
            schedule_subscription_retry(std::chrono::seconds(5));
        }
    }
}

void OpcTransport::schedule_subscription_retry(std::chrono::milliseconds delay) {
    if (!keep_running_) return;
    subscription_timer_.expires_after(delay);
    subscription_timer_.async_wait([this, alive = std::weak_ptr<bool>(alive_)](boost::system::error_code ec) {
        auto a = alive.lock();
        if (ec || !a || !*a || !keep_running_ || !session_active_) return;
        subscription_pending_ = false;
        create_subscription();
    });
}

// ------------------------------------------------------------------ чтение и запись

void OpcTransport::read_nodes(std::span<const opcua::NodeId> nodes, ReadDone done) {
    if (!session_active_ || nodes.empty()) {
        call_user("read", done, std::nullopt);
        return;
    }
    std::vector<opcua::ReadValueId> items;
    items.reserve(nodes.size());
    for (const auto& id : nodes) items.emplace_back(id, opcua::AttributeId::Value);

    try {
        opcua::services::readAsync(
            client_, items, opcua::TimestampsToReturn::Neither,
            [this, alive = std::weak_ptr<bool>(alive_), done, count = nodes.size()](opcua::ReadResponse& response) {
                auto a = alive.lock();
                if (!a || !*a) return;         // закрываемся — объекты моста уже могут быть удалены
                const auto service = response.responseHeader().serviceResult();
                auto results = response.results();
                if (service.isBad() || results.size() != count) {
                    spdlog::error("[{}] Read failed: {}", name_, service.name());
                    call_user("read", done, std::nullopt);
                    return;
                }
                std::vector<opcua::Variant> values;
                values.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    if (!results[i].status().isGood()) {
                        spdlog::error("[{}] Read item {} failed: {}", name_, i, results[i].status().name());
                        call_user("read", done, std::nullopt);
                        return;
                    }
                    values.push_back(std::move(results[i].value()));
                }
                call_user("read", done, std::optional(std::move(values)));
            });
    } catch (const std::exception& e) {             // например, соединение уже закрыто
        spdlog::error("[{}] Read not sent: {}", name_, e.what());
        call_user("read", done, std::nullopt);
    }
}

void OpcTransport::write_nodes(std::span<const opcua::NodeId> nodes, std::span<const opcua::Variant> values,
                               WriteDone done) {
    write_nodes_each(nodes, values, [done = std::move(done)](std::vector<bool> results) {
        const bool ok = !results.empty() && std::all_of(results.begin(), results.end(), [](bool r) { return r; });
        call_user("write", done, ok);
    });
}

void OpcTransport::write_nodes_each(std::span<const opcua::NodeId> nodes, std::span<const opcua::Variant> values,
                                    WriteEachDone done) {
    if (!session_active_ || nodes.empty() || nodes.size() != values.size()) {
        call_user("write", done, std::vector<bool>{});
        return;
    }
    std::vector<opcua::WriteValue> items;
    items.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        // только значение, без меток времени: часть ПЛК отклоняет запись с ними (BadWriteNotSupported)
        items.emplace_back(nodes[i], opcua::AttributeId::Value, std::string_view{}, opcua::DataValue(values[i]));
    }

    try {
        opcua::services::writeAsync(
            client_, items,
            [this, alive = std::weak_ptr<bool>(alive_), done,
             names = std::vector<opcua::NodeId>(nodes.begin(), nodes.end())](opcua::WriteResponse& response) {
                auto a = alive.lock();
                if (!a || !*a) return;
                const auto service = response.responseHeader().serviceResult();
                auto results = response.results();
                if (service.isBad() || results.size() != names.size()) {
                    spdlog::error("[{}] Write failed: {}", name_, service.name());
                    call_user("write", done, std::vector<bool>{});
                    return;
                }
                std::vector<bool> ok(names.size());
                for (size_t i = 0; i < names.size(); ++i) {
                    ok[i] = results[i].isGood();
                    if (!ok[i]) spdlog::error("[{}] Write {} failed: {}", name_, node_name(names[i]), results[i].name());
                }
                call_user("write", done, std::move(ok));
            });
    } catch (const std::exception& e) {
        spdlog::error("[{}] Write not sent: {}", name_, e.what());
        call_user("write", done, std::vector<bool>{});
    }
}

void OpcTransport::read_node(const opcua::NodeId& node_id,
                             std::function<void(std::optional<opcua::Variant>)> done) {
    read_nodes(std::span(&node_id, 1), [done = std::move(done)](std::optional<std::vector<opcua::Variant>> values) {
        if (!done) return;
        if (values && values->size() == 1) {
            done(std::move(values->front()));
        } else {
            done(std::nullopt);
        }
    });
}

void OpcTransport::write_node(const opcua::NodeId& node_id, const opcua::Variant& value, WriteDone done) {
    write_nodes(std::span(&node_id, 1), std::span(&value, 1), std::move(done));
}

// ------------------------------------------------------------------ совместимость со старым мостом

void OpcTransport::read_from_node(const opcua::NodeId& node_id,
                                  std::function<void(const opcua::NodeId&, opcua::Variant)> on_complete) {
    read_node(node_id, [node_id, on_complete = std::move(on_complete)](std::optional<opcua::Variant> value) {
        if (value && on_complete) on_complete(node_id, std::move(*value));
    });
}

void OpcTransport::write_to_node(const opcua::NodeId& node_id, const opcua::Variant& value,
                                 std::function<void()> on_complete) {
    write_node(node_id, value, [on_complete = std::move(on_complete)](bool ok) {
        if (ok && on_complete) on_complete();
    });
}
