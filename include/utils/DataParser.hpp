
#ifndef SERIALDRIVER_SERIALPARSER_HPP
#define SERIALDRIVER_SERIALPARSER_HPP

#include <array>
#include <vector>
#include <cstdint>
#include <optional>
#include <span>
#pragma pack(push, 1)

template <typename T>
    struct SerialFrame {
    uint8_t header{0xAA};
    uint8_t device_id{0};
    uint8_t cmd_id{0};
    uint8_t length{0};
    T data{};
    uint8_t crc8{0};
    uint8_t stopByte{0x55};
};

struct CommandData {
    uint8_t transaction_id{0};
    uint8_t axis_id;
    uint8_t powerOn;
    std::array<float, 3> position;
    float velocity;
    float acceleration;
    float deceleration;
    std::array<int8_t, 3> direction;
};

struct StatusData {
    uint8_t transaction_id{0}; // 1
    std::array<float, 3> position{0.0f, 0.0f, 0.0f}; // 12
    std::array<uint8_t, 3> state_id{0, 0, 0}; // 3
    std::array<uint8_t, 3> is_home{0, 0, 0};// 3
    std::array<uint8_t, 3> is_powered{0, 0, 0};// 3
    std::array<uint8_t, 3> is_target{0, 0, 0}; // 3
    std::array<uint8_t, 3> in_motion{0, 0, 0}; // 3
    std::array<uint8_t, 3> error_id{0, 0, 0}; // 3
};
#pragma pack(pop)

class DataParser {
public:
    uint8_t calculate_crc(const uint8_t *data, size_t len);
    std::vector<uint8_t> struct_to_binary(SerialFrame<CommandData>& data);
    std::optional<SerialFrame<StatusData>> binary_to_struct(std::span<const uint8_t> data);
};


#endif //SERIALDRIVER_SERIALPARSER_HPP
