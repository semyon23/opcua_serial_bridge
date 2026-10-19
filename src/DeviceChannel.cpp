#include "../include/DeviceChannel.hpp"

#include <spdlog/spdlog.h>


namespace {

constexpr auto kRetryDelay = std::chrono::milliseconds(200);

const char* command_name(uint8_t cmd) {
    switch (cmd) {
        case proto::POWER_ENABLE: return "POWER_ENABLE";
        case proto::MOVE_RELATIVE: return "MOVE_RELATIVE";
        case proto::MOVE_ABSOLUTE: return "MOVE_ABSOLUTE";
        case proto::MOVE_CONTINUE: return "MOVE_CONTINUE";
        case proto::MOVE_HOME: return "MOVE_HOME";
        case proto::HALT: return "HALT";
        case proto::EMERGENCY_STOP: return "EMERGENCY_STOP";
        case proto::SET_POSITION: return "SET_POSITION";
        case proto::RESET_ERROR: return "RESET_ERROR";
        default: return "?";
    }
}

}  // namespace

DeviceChannel::DeviceChannel(asio::io_context& io, OpcTransport& opc, PortWorker& port, PortPublisher& publisher,
                             DeviceNodes nodes, const DeviceConfig& config, const NodeOptions& options,
                             std::string name)
    : opc_(opc),
      port_(port),
      publisher_(publisher),
      nodes_(std::move(nodes)),
      config_(config),
      options_(options),
      name_(std::move(name)),
      retry_timer_(io),
      recheck_timer_(io) {
    command_read_.push_back(nodes_.cmd_ready);
    command_read_.insert(command_read_.end(), nodes_.control.begin(), nodes_.control.end());
}

void DeviceChannel::start() {
    running_ = true;
    opc_.register_node(nodes_.cmd_ready, [this](const opcua::Variant& value) { on_cmd_ready(value); });
}

void DeviceChannel::stop() {
    running_ = false;
    retry_timer_.cancel();
    recheck_timer_.cancel();
}

// ------------------------------------------------------------------ команда из ПЛК

void DeviceChannel::on_cmd_ready(const opcua::Variant& value) {
    if (!running_ || !value.isScalar() || !value.isType<bool>() || !value.to<bool>()) return;
    if (command_busy_) return;           // прошлая команда ещё не закрыта сбросом cmdReady
    command_busy_ = true;

    // cmdReady читаем вместе с control.*: уведомление могло устареть, пока шло
    opc_.read_nodes(command_read_, [this](std::optional<std::vector<opcua::Variant>> values) {
        if (!running_) return;
        if (!values) {
            spdlog::error("[{}] cannot read control nodes", name_);
            finish_command(CommandResult::ReadFailed);
            return;
        }
        const auto& ready = values->front();
        if (!ready.isScalar() || !ready.isType<bool>() || !ready.to<bool>()) {
            command_busy_ = false;          // устаревшее уведомление: команду уже закрыли
            return;
        }
        auto decoded = decode_command(std::span(*values).subspan(1), options_.deceleration);
        if (auto* error = std::get_if<std::string>(&decoded)) {
            spdlog::error("[{}] command rejected: {}", name_, *error);
            finish_command(CommandResult::Rejected);
            return;
        }
        const auto command = std::get<PlcCommand>(decoded);
        const auto& d = command.data;
        spdlog::info("[{}] {} tid={} axis={} pwr={} pos={:.2f},{:.2f},{:.2f} v={:.1f} a={:.1f} d={:.1f} dir={},{},{}",
                     name_, command_name(command.cmd), d.transaction_id, d.axis_id, d.power_enable, d.position[0],
                     d.position[1], d.position[2], d.velocity, d.acceleration, d.deceleration, d.direction[0],
                     d.direction[1], d.direction[2]);

        port_.submit(config_.address, command.cmd, proto::pack(command.data), Priority::Command,
                     [this](std::optional<Reply> reply) {
                         if (!running_) return;
                         auto status = parse_status(reply);
                         if (!status) {
                             spdlog::warn("[{}] no reply to the command", name_);
                             finish_command(CommandResult::NoReply);
                             return;
                         }
                         // cmdReady сбрасываем, когда ответ (новый transaction_id) уже в ПЛК
                         publish_status(*status, true, [this](bool) {
                             if (running_) finish_command(CommandResult::Ok);
                         });
                     });
    });
}

void DeviceChannel::finish_command(CommandResult result) {
    // cmdResult — отдельным запросом и раньше cmdReady: ПЛК читает итог, увидев cmdReady = false
    auto reset_ready = [this, result] {
        opc_.write_node(nodes_.cmd_ready, opcua::Variant(false), [this, result](bool ok) {
            if (!running_) return;
            if (ok) {
                command_busy_ = false;
                schedule_recheck();
                return;
            }
            // не записалось (нет связи, ошибка ПЛК) — повторять, иначе cmdReady останется true навсегда
            retry_timer_.expires_after(kRetryDelay);
            retry_timer_.async_wait([this, result](boost::system::error_code ec) {
                if (!ec && running_) finish_command(result);
            });
        });
    };
    if (!nodes_.cmd_result) {
        reset_ready();
        return;
    }
    opc_.write_node(*nodes_.cmd_result, opcua::Variant(static_cast<uint8_t>(result)),
                    [reset_ready](bool) { reset_ready(); });
}

// Если ПЛК поднял cmdReady быстрее, чем сервер опрашивает значение (true → false → true за один
// интервал), уведомления не будет. Поэтому после сброса один раз перечитываем cmdReady сами —
// когда сервер уже точно успел бы заметить изменение.
void DeviceChannel::schedule_recheck() {
    recheck_timer_.expires_after(opc_.change_detection_time() + std::chrono::milliseconds(50));
    recheck_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec || !running_ || command_busy_) return;
        opc_.read_node(nodes_.cmd_ready, [this](std::optional<opcua::Variant> value) {
            if (running_ && value) on_cmd_ready(*value);
        });
    });
}

// ------------------------------------------------------------------ опрос статуса

bool DeviceChannel::poll_due(uint64_t cycle, std::chrono::milliseconds cycle_period) const {
    const auto interval = online_ ? config_.poll.status_poll : config_.poll.offline_poll;
    const auto every = std::max<int64_t>(1, (interval.count() + cycle_period.count() / 2) / cycle_period.count());
    return cycle % static_cast<uint64_t>(every) == 0;
}

void DeviceChannel::poll(std::function<void()> done) {
    if (!running_) {
        done();
        return;
    }
    port_.submit(config_.address, proto::GET_STATUS, {0}, Priority::Poll,
                 [this, done = std::move(done)](std::optional<Reply> reply) {
                     if (running_) on_poll_reply(std::move(reply));
                     done();
                 });
}

void DeviceChannel::on_poll_reply(std::optional<Reply> reply) {
    if (auto status = parse_status(reply)) {
        timeouts_in_row_ = 0;
        set_online(true);
        publish_status(*status, false);
        return;
    }
    if (++timeouts_in_row_ >= config_.poll.offline_after) set_online(false);
}

std::optional<proto::StatusData> DeviceChannel::parse_status(const std::optional<Reply>& reply) {
    if (!reply) return std::nullopt;
    auto status = proto::unpack_status(reply->payload);
    if (!status) spdlog::warn("[{}] reply with {} bytes instead of StatusData", name_, reply->payload.size());
    return status;
}

// urgent — ответ на команду: пишется всегда и сразу (ПЛК ждёт новый transaction_id).
// Статус опроса — только если изменился, плюс раз в status_refresh, если ПЛК перезапускался.
void DeviceChannel::publish_status(const proto::StatusData& status, bool urgent, PortPublisher::Sent sent) {
    const bool unchanged = last_published_ && *last_published_ == status;
    const bool fresh = Clock::now() - last_publish_time_ < config_.poll.status_refresh;
    if (!urgent && unchanged && fresh) return;      // ничего нового — ПЛК не нагружаем
    last_published_ = status;
    last_publish_time_ = Clock::now();
    publisher_.publish(nodes_, status, urgent, std::move(sent));
}

void DeviceChannel::set_online(bool online) {
    if (online == online_ && online_published_) return;
    if (online != online_) {
        if (online) spdlog::info("[{}] device answers again", name_);
        else spdlog::warn("[{}] device offline: {} polls without reply", name_, timeouts_in_row_);
    }
    online_ = online;
    if (!nodes_.online) {
        online_published_ = true;
        return;
    }
    opc_.write_node(*nodes_.online, opcua::Variant(online), [this, online](bool ok) {
        if (running_ && ok && online == online_) online_published_ = true;
    });
}
