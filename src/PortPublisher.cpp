#include "../include/PortPublisher.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

PortPublisher::PortPublisher(asio::io_context& io, OpcTransport& opc, std::chrono::milliseconds flush_interval,
                             size_t max_nodes_per_request, std::string name)
    : opc_(opc),
      timer_(io),
      flush_interval_(flush_interval),
      max_devices_per_batch_(std::max<size_t>(1, max_nodes_per_request / StatusNodeCount)),
      name_(std::move(name)) {}

void PortPublisher::start() {
    running_ = true;
    if (flush_interval_.count() > 0) schedule_tick();
}

void PortPublisher::stop() {
    running_ = false;
    timer_.cancel();
    pending_.clear();
}

void PortPublisher::publish(const DeviceNodes& nodes, const proto::StatusData& status, bool urgent, Sent sent) {
    if (!running_) {
        if (sent) sent(false);
        return;
    }
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Item& i) { return i.nodes == &nodes; });
    if (it == pending_.end()) {
        it = pending_.insert(pending_.end(), Item{&nodes, status, urgent, {}});
    } else {
        it->status = status;            // в ПЛК нужен только последний статус
        it->urgent = it->urgent || urgent;
    }
    if (sent) it->sent.push_back(std::move(sent));
    if (urgent) flush();                // если пачка уже пишется — уйдёт сразу после неё
}

void PortPublisher::schedule_tick() {
    timer_.expires_after(flush_interval_);
    timer_.async_wait([this](boost::system::error_code ec) {
        if (ec || !running_) return;
        flush();
        schedule_tick();
    });
}

void PortPublisher::flush() {
    // без сессии писать некуда: статусы ждут переподключения и уйдут со следующей отправкой
    if (!running_ || in_flight_ || pending_.empty() || !opc_.session_active()) return;

    // лимит сервера на число узлов в запросе: что не влезло — следующей пачкой
    const size_t count = std::min(pending_.size(), max_devices_per_batch_);
    std::vector<Item> batch(std::make_move_iterator(pending_.begin()),
                            std::make_move_iterator(pending_.begin() + static_cast<long>(count)));
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(count));
    left_behind_ = !pending_.empty();

    std::vector<opcua::NodeId> nodes;
    std::vector<opcua::Variant> values;
    for (const Item& item : batch) {
        auto v = encode_status(item.status);
        nodes.insert(nodes.end(), item.nodes->status.begin(), item.nodes->status.end());
        values.insert(values.end(), v.begin(), v.end());
    }

    in_flight_ = true;
    ++stats_.batches;
    stats_.statuses += batch.size();
    opc_.write_nodes_each(nodes, values, [this, batch = std::move(batch)](std::vector<bool> ok) mutable {
        if (!running_) return;
        if (ok.empty()) {                                   // запрос не прошёл целиком — повторить позже
            ++stats_.failed;
            for (Item& item : batch) put_back(std::move(item));
            in_flight_ = false;
            return;
        }
        // плата записана, если записались все её узлы; ошибку узла транспорт уже написал в журнал
        std::vector<Item> written;
        for (size_t i = 0; i < batch.size(); ++i) {
            const auto first = ok.begin() + static_cast<long>(i * StatusNodeCount);
            if (std::all_of(first, first + StatusNodeCount, [](bool r) { return r; })) {
                written.push_back(std::move(batch[i]));
            } else {
                for (auto& sent : batch[i].sent) sent(false);
            }
        }
        write_ready(std::move(written));
    });
}

// infoReady — отдельным запросом и только после значений: так ПЛК не увидит флаг раньше данных.
void PortPublisher::write_ready(std::vector<Item> batch) {
    if (batch.empty()) {
        batch_finished();
        return;
    }
    std::vector<opcua::NodeId> nodes;
    for (const Item& item : batch) nodes.push_back(item.nodes->info_ready);
    std::vector<opcua::Variant> values(nodes.size(), opcua::Variant(true));

    opc_.write_nodes_each(nodes, values, [this, batch = std::move(batch)](std::vector<bool> ok) mutable {
        if (!running_) return;
        if (ok.empty()) {
            ++stats_.failed;
            for (Item& item : batch) put_back(std::move(item));
        } else {
            for (size_t i = 0; i < batch.size(); ++i) {
                for (auto& sent : batch[i].sent) sent(ok[i]);
            }
        }
        batch_finished();
    });
}

// Следующая пачка — сразу, если ждёт ответ на команду или прошлая пачка не вместила всех.
// Иначе — по таймеру: так в пачку успевают собраться статусы нескольких плат.
void PortPublisher::batch_finished() {
    in_flight_ = false;
    const bool urgent = std::any_of(pending_.begin(), pending_.end(), [](const Item& i) { return i.urgent; });
    if (urgent || left_behind_) flush();
}

// Пачка не дошла: вернуть статус в очередь, если за это время не пришёл новее.
// Кто ждал этот статус, узнаёт о неудаче сразу — повтор уже без него.
void PortPublisher::put_back(Item item) {
    for (auto& sent : item.sent) sent(false);
    item.sent.clear();
    item.urgent = false;
    const bool newer = std::any_of(pending_.begin(), pending_.end(), [&](const Item& i) { return i.nodes == item.nodes; });
    if (!newer) pending_.insert(pending_.begin(), std::move(item));
}
