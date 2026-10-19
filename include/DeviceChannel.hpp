#pragma once

#include "Config.hpp"
#include "DeviceNodes.hpp"
#include "OpcTransport.hpp"
#include "PortPublisher.hpp"
#include "PortWorker.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

// Одно устройство: команды из ПЛК → плата, статус платы → ПЛК.
// Знает свой порт и свой адрес; ни с другими устройствами, ни с портами не связан.
class DeviceChannel {
public:
    DeviceChannel(asio::io_context& io, OpcTransport& opc, PortWorker& port, PortPublisher& publisher,
                  DeviceNodes nodes, const DeviceConfig& config, const NodeOptions& options, std::string name);
    DeviceChannel(const DeviceChannel&) = delete;
    DeviceChannel& operator=(const DeviceChannel&) = delete;

    void start();       // до opc.run(): регистрирует cmdReady в подписке
    void stop();

    const std::string& name() const { return name_; }
    bool online() const { return online_; }

    // Опрос статуса — по команде PortPoller, кругами порта.
    // Пора ли в этом круге: у платы свой период (status_poll_ms, или offline_poll_ms, пока не отвечает),
    // округлённый до целого числа кругов.
    bool poll_due(uint64_t cycle, std::chrono::milliseconds cycle_period) const;
    // GET_STATUS в очередь порта; done — когда опрос закончен: ответ или тайм-аут (вызывается всегда).
    void poll(std::function<void()> done);

private:
    using Clock = std::chrono::steady_clock;

    void on_cmd_ready(const opcua::Variant& value);
    void finish_command(CommandResult result);
    void schedule_recheck();
    void on_poll_reply(std::optional<Reply> reply);
    std::optional<proto::StatusData> parse_status(const std::optional<Reply>& reply);
    void publish_status(const proto::StatusData& status, bool urgent, PortPublisher::Sent sent = {});
    void set_online(bool online);

    OpcTransport& opc_;
    PortWorker& port_;
    PortPublisher& publisher_;
    DeviceNodes nodes_;
    DeviceConfig config_;
    NodeOptions options_;
    std::string name_;                          // «port 1 / device 2» — для журнала
    asio::steady_timer retry_timer_;
    asio::steady_timer recheck_timer_;
    std::vector<opcua::NodeId> command_read_;   // cmdReady + control.* — одним запросом

    bool running_{false};
    bool command_busy_{false};                  // команда в работе: повторный cmdReady не принимаем
    bool online_{true};
    bool online_published_{false};
    int timeouts_in_row_{0};

    std::optional<proto::StatusData> last_published_;   // последний статус, отданный сборщику
    Clock::time_point last_publish_time_{};
};
