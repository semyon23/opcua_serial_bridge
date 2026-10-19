#include "../include/protocol.hpp"

#include <bit>
#include <cstring>
#include <type_traits>

namespace proto {

namespace {

std::array<uint8_t, 256> make_crc_table() {
    std::array<uint8_t, 256> table{};
    for (unsigned byte = 0; byte < 256; ++byte) {
        auto crc = static_cast<uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
        table[byte] = crc;
    }
    return table;
}

// Поле за полем, без выравнивания: так поля лежат в кадре.
class Writer {
public:
    explicit Writer(size_t size) { bytes_.reserve(size); }
    template <typename T>
    void put(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        bytes_.insert(bytes_.end(), p, p + sizeof(T));
    }
    template <typename T, size_t N>
    void put(const std::array<T, N>& values) {
        for (const auto& v : values) put(v);
    }
    std::vector<uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    template <typename T>
    void get(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::memcpy(&value, bytes_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
    }
    template <typename T, size_t N>
    void get(std::array<T, N>& values) {
        for (auto& v : values) get(v);
    }

private:
    std::span<const uint8_t> bytes_;
    size_t pos_{0};
};

static_assert(std::endian::native == std::endian::little, "protocol: little-endian host expected");

}  // namespace

std::vector<uint8_t> pack(const CommandData& d) {
    Writer w(COMMAND_DATA_SIZE);
    w.put(d.transaction_id);
    w.put(d.axis_id);
    w.put(d.power_enable);
    w.put(d.position);
    w.put(d.velocity);
    w.put(d.acceleration);
    w.put(d.deceleration);
    w.put(d.direction);
    return w.take();
}

std::vector<uint8_t> pack(const StatusData& d) {
    Writer w(STATUS_DATA_SIZE);
    w.put(d.transaction_id);
    w.put(d.position);
    w.put(d.state);
    w.put(d.is_home);
    w.put(d.is_powered);
    w.put(d.is_target);
    w.put(d.in_motion);
    w.put(d.error_id);
    return w.take();
}

std::vector<uint8_t> pack(const FeedbackData& d) {
    Writer w(FEEDBACK_DATA_SIZE);
    for (const auto* a : {&d.position, &d.feedback_position, &d.lag_error, &d.velocity, &d.velocity_feedback}) w.put(*a);
    for (const auto* a : {&d.in_motion, &d.in_target, &d.error, &d.error_id, &d.state, &d.negative_limit,
                          &d.positive_limit}) {
        w.put(*a);
    }
    return w.take();
}

std::optional<CommandData> unpack_command(std::span<const uint8_t> payload) {
    if (payload.size() != COMMAND_DATA_SIZE) return std::nullopt;
    CommandData d;
    Reader r(payload);
    r.get(d.transaction_id);
    r.get(d.axis_id);
    r.get(d.power_enable);
    r.get(d.position);
    r.get(d.velocity);
    r.get(d.acceleration);
    r.get(d.deceleration);
    r.get(d.direction);
    return d;
}

std::optional<StatusData> unpack_status(std::span<const uint8_t> payload) {
    if (payload.size() != STATUS_DATA_SIZE) return std::nullopt;
    StatusData d;
    Reader r(payload);
    r.get(d.transaction_id);
    r.get(d.position);
    r.get(d.state);
    r.get(d.is_home);
    r.get(d.is_powered);
    r.get(d.is_target);
    r.get(d.in_motion);
    r.get(d.error_id);
    return d;
}

std::optional<FeedbackData> unpack_feedback(std::span<const uint8_t> payload) {
    if (payload.size() != FEEDBACK_DATA_SIZE) return std::nullopt;
    FeedbackData d;
    Reader r(payload);
    for (auto* a : {&d.position, &d.feedback_position, &d.lag_error, &d.velocity, &d.velocity_feedback}) r.get(*a);
    for (auto* a : {&d.in_motion, &d.in_target, &d.error, &d.error_id, &d.state, &d.negative_limit,
                    &d.positive_limit}) {
        r.get(*a);
    }
    return d;
}

uint8_t crc8(std::span<const uint8_t> data) {
    static const auto table = make_crc_table();
    uint8_t crc = 0;
    for (uint8_t byte : data) crc = table[crc ^ byte];
    return crc;
}

std::vector<uint8_t> encode_frame(uint8_t address, uint8_t cmd, std::span<const uint8_t> payload) {
    std::vector<uint8_t> frame;
    frame.reserve(payload.size() + FRAME_OVERHEAD);
    frame.push_back(FRAME_HEADER);
    frame.push_back(address);
    frame.push_back(cmd);
    frame.push_back(static_cast<uint8_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(crc8(frame));
    frame.push_back(FRAME_FOOTER);
    return frame;
}

std::optional<Frame> FrameParser::feed(uint8_t byte) {
    if (buf_.empty() && byte != FRAME_HEADER) return std::nullopt;
    if (buf_.size() == 3 && byte > MAX_PAYLOAD) {          // такой длины не бывает — ложный заголовок
        buf_.clear();
        return std::nullopt;
    }
    buf_.push_back(byte);
    if (buf_.size() < 4 || buf_.size() != FRAME_OVERHEAD + buf_[3]) return std::nullopt;

    std::vector<uint8_t> raw;
    raw.swap(buf_);
    if (raw.back() != FRAME_FOOTER) return std::nullopt;
    if (crc8(std::span(raw).first(raw.size() - 2)) != raw[raw.size() - 2]) {
        ++crc_errors_;
        return std::nullopt;
    }
    Frame frame;
    frame.address = raw[1];
    frame.cmd = raw[2];
    frame.payload.assign(raw.begin() + 4, raw.end() - 2);
    frame.raw = std::move(raw);
    return frame;
}

}  // namespace proto
