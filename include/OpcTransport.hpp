#pragma once

#include <open62541pp/open62541pp.hpp>
#include <boost/asio.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace asio = boost::asio;

struct OpcSettings {
    std::string endpoint;
    uint32_t request_timeout_ms{1000};

    opcua::MessageSecurityMode security_mode{opcua::MessageSecurityMode::None};
    std::string username{};                     // пусто — анонимный вход
    std::string password{};

    // Подписка. Время жизни = lifetime_count × publishing_interval: 600 × 50 мс = 30 с.
    // Сервер может поправить значения под свои пределы.
    double publishing_interval_ms{50.0};
    uint32_t lifetime_count{600};
    uint32_t max_keep_alive_count{20};          // keep-alive раз в 20 × 50 мс = 1 с
    double sampling_interval_ms{-1.0};          // −1 — как publishing_interval
    uint32_t queue_size{10};                    // чтобы быстрые true→false→true не терялись

    std::chrono::milliseconds loop_interval{2};             // как часто вызывать runIterate
    std::chrono::milliseconds slow_iterate_warning{5};      // runIterate дольше — пишем в журнал
    std::vector<std::chrono::milliseconds> reconnect_backoff{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5), std::chrono::seconds(10)};
};

// OPC UA-клиент поверх io_context. Всё — в потоке, который крутит io_context:
// методы вызывать только из него, колбэки тоже приходят в нём.
// Объект должен жить, пока работает io_context.
class OpcTransport {
public:
    using DataChangeCallback = std::function<void(const opcua::Variant&)>;
    // Колбэки чтения и записи вызываются ВСЕГДА — с результатом или с ошибкой.
    using ReadDone = std::function<void(std::optional<std::vector<opcua::Variant>>)>;
    using WriteDone = std::function<void(bool ok)>;
    // Итог по каждому узлу в том же порядке; пустой — запрос не прошёл целиком (нет связи, тайм-аут).
    using WriteEachDone = std::function<void(std::vector<bool> ok)>;

    OpcTransport(asio::io_context& ioc, OpcSettings settings);
    OpcTransport(asio::io_context& ioc, const std::string& endpoint);
    ~OpcTransport();

    OpcTransport(const OpcTransport&) = delete;
    OpcTransport& operator=(const OpcTransport&) = delete;

    void run();
    void stop();
    bool session_active() const;

    // За сколько сервер гарантированно заметит изменение значения: max(выборка, публикация),
    // как он их выставил. Два изменения быстрее этого (true → false → true) он может не заметить.
    std::chrono::milliseconds change_detection_time() const;

    // Узлы для подписки. Регистрировать до run(); после переподключения подписка
    // создаётся заново, и сервер снова присылает текущие значения.
    void register_node(const opcua::NodeId& node_id, DataChangeCallback callback);

    // Один запрос Read / Write на все узлы.
    void read_nodes(std::span<const opcua::NodeId> nodes, ReadDone done);
    void write_nodes(std::span<const opcua::NodeId> nodes, std::span<const opcua::Variant> values,
                     WriteDone done);
    void write_nodes_each(std::span<const opcua::NodeId> nodes, std::span<const opcua::Variant> values,
                          WriteEachDone done);
    void read_node(const opcua::NodeId& node_id, std::function<void(std::optional<opcua::Variant>)> done);
    void write_node(const opcua::NodeId& node_id, const opcua::Variant& value, WriteDone done);

    // Старые методы — чтобы текущий OpcSerialBridge собрался без правок.
    // Колбэк, как и раньше, только при успехе; новый код пусть использует read_nodes / write_nodes.
    void read_from_node(const opcua::NodeId& node_id,
                        std::function<void(const opcua::NodeId&, opcua::Variant)> on_complete);
    void write_to_node(const opcua::NodeId& node_id, const opcua::Variant& value,
                       std::function<void()> on_complete);

private:
    void configure_client();
    void connect();
    void on_session_activated();
    void on_connection_lost(std::string_view reason);
    void schedule_reconnect();
    asio::awaitable<void> run_loop();

    void create_subscription();
    void schedule_subscription_retry(std::chrono::milliseconds delay);
    void on_subscription_created(opcua::CreateSubscriptionResponse& response);

    const std::string name_{"OPC UA"};
    OpcSettings settings_;
    asio::io_context& io_;
    asio::steady_timer loop_timer_;
    asio::steady_timer reconnect_timer_;
    asio::steady_timer subscription_timer_;

    bool keep_running_{false};
    bool session_active_{false};
    bool reconnect_pending_{false};
    size_t reconnect_attempt_{0};

    std::map<opcua::NodeId, DataChangeCallback> node_callbacks_;
    std::optional<opcua::IntegerId> subscription_id_;
    bool subscription_pending_{false};
    uint64_t subscription_generation_{0};     // ответы на старые запросы подписки отбрасываем
    double revised_publishing_ms_{0};
    double revised_sampling_ms_{0};

    // Отложенные обработчики (post, таймеры) проверяют его: объект мог быть уже удалён.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};

    // Последним: уничтожается первым, пока остальные поля ещё живы —
    // его колбэки при закрытии могут обратиться к ним.
    opcua::Client client_;
};
