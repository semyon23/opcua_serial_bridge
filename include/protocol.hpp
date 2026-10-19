#pragma once

// Бинарный протокол плат StepperDriver — общий договор с прошивкой (include/CommandManager.hpp).
// Только стандартная библиотека: файл можно подключать и в прошивку.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace proto {

enum Command : uint8_t {
    POWER_ENABLE = 1,
    MOVE_RELATIVE = 2,
    MOVE_ABSOLUTE = 3,
    MOVE_CONTINUE = 4,
    MOVE_HOME = 5,
    HALT = 6,
    EMERGENCY_STOP = 7,
    SET_POSITION = 8,
    GET_STATUS = 10,
    GET_FEEDBACK = 20,
    RESET_ERROR = 30,
    CHANGE_ID_DEVICE = 250,
    JUMP_TO_BOOTLOADER = 255,
};

constexpr uint8_t AXIS_GROUP = 3;
constexpr size_t AXIS_COUNT = 3;

// Структуры для работы в программе — обычные, выровненные. В кадре поля лежат подряд без
// выравнивания (как #pragma pack(1) в прошивке): в байты и обратно — только через pack / unpack.
// Обращаться к полям упакованной структуры напрямую нельзя: float со смещением 1 — невыровненный доступ.
struct CommandData {
    uint8_t transaction_id{0};
    uint8_t axis_id{0};
    uint8_t power_enable{0};
    std::array<float, AXIS_COUNT> position{};
    float velocity{0};
    float acceleration{0};
    float deceleration{0};
    std::array<int8_t, AXIS_COUNT> direction{};
    bool operator==(const CommandData&) const = default;
};

struct StatusData {
    uint8_t transaction_id{0};
    std::array<float, AXIS_COUNT> position{};
    std::array<uint8_t, AXIS_COUNT> state{};
    std::array<uint8_t, AXIS_COUNT> is_home{};
    std::array<uint8_t, AXIS_COUNT> is_powered{};
    std::array<uint8_t, AXIS_COUNT> is_target{};
    std::array<uint8_t, AXIS_COUNT> in_motion{};
    std::array<uint8_t, AXIS_COUNT> error_id{};
    bool operator==(const StatusData&) const = default;
};

struct FeedbackData {
    std::array<float, AXIS_COUNT> position{};
    std::array<float, AXIS_COUNT> feedback_position{};
    std::array<float, AXIS_COUNT> lag_error{};
    std::array<float, AXIS_COUNT> velocity{};
    std::array<float, AXIS_COUNT> velocity_feedback{};
    std::array<uint8_t, AXIS_COUNT> in_motion{};
    std::array<uint8_t, AXIS_COUNT> in_target{};
    std::array<uint8_t, AXIS_COUNT> error{};
    std::array<uint8_t, AXIS_COUNT> error_id{};
    std::array<uint8_t, AXIS_COUNT> state{};
    std::array<uint8_t, AXIS_COUNT> negative_limit{};
    std::array<uint8_t, AXIS_COUNT> positive_limit{};
    bool operator==(const FeedbackData&) const = default;
};

// Размеры в кадре — как sizeof упакованных структур прошивки (include/CommandManager.hpp)
constexpr size_t COMMAND_DATA_SIZE = 3 + 3 * 4 + 3 * 4 + 3;     // 30
constexpr size_t STATUS_DATA_SIZE = 1 + 3 * 4 + 6 * 3;          // 31
constexpr size_t FEEDBACK_DATA_SIZE = 15 * 4 + 7 * 3;           // 81
static_assert(COMMAND_DATA_SIZE == 30 && STATUS_DATA_SIZE == 31 && FEEDBACK_DATA_SIZE == 81);

// ------------------------------------------------------------------ данные

// little-endian, как у Cortex-M4 и x86. unpack_* — nullopt, если длина не та.
std::vector<uint8_t> pack(const CommandData& data);
std::vector<uint8_t> pack(const StatusData& data);
std::vector<uint8_t> pack(const FeedbackData& data);
std::optional<CommandData> unpack_command(std::span<const uint8_t> payload);
std::optional<StatusData> unpack_status(std::span<const uint8_t> payload);
std::optional<FeedbackData> unpack_feedback(std::span<const uint8_t> payload);

// ------------------------------------------------------------------ кадр: 0xAA | адрес | команда | LEN | данные | CRC8 | 0x55

constexpr uint8_t FRAME_HEADER = 0xAA;
constexpr uint8_t FRAME_FOOTER = 0x55;
constexpr size_t FRAME_OVERHEAD = 6;
constexpr size_t MAX_PAYLOAD = 250;          // буфер прошивки 256 байт минус заголовок и хвост

uint8_t crc8(std::span<const uint8_t> data);  // полином 0x31, начальное значение 0
std::vector<uint8_t> encode_frame(uint8_t address, uint8_t cmd, std::span<const uint8_t> payload);

struct Frame {
    uint8_t address{0};
    uint8_t cmd{0};
    std::vector<uint8_t> payload;
    std::vector<uint8_t> raw;                 // кадр целиком — чтобы узнать эхо своего же запроса
};

// Побайтовый разбор как в прошивке: заголовок, длина, CRC, хвост.
class FrameParser {
public:
    std::optional<Frame> feed(uint8_t byte);
    void reset() { buf_.clear(); }
    uint64_t crc_errors() const { return crc_errors_; }

private:
    std::vector<uint8_t> buf_;
    uint64_t crc_errors_{0};
};

}  // namespace proto
