
#include "../../include/utils/DataParser.hpp"
#include <cstring>
#include <spdlog/spdlog.h>
std::vector<uint8_t> DataParser::struct_to_binary(SerialFrame<CommandData> &data) {
    data.length = static_cast<uint8_t>(sizeof(data.data));
    const uint8_t* pkgPtr = reinterpret_cast<const uint8_t*>(&data);
    std::vector<uint8_t> buffer(pkgPtr, pkgPtr + sizeof(SerialFrame<CommandData>));

    auto size_for_crc = sizeof(SerialFrame<CommandData>) - 2;
    uint8_t calculated_crc = calculate_crc(buffer.data(), size_for_crc);

    buffer[sizeof(SerialFrame<CommandData>) - 2] = calculated_crc;
    return buffer;
}

std::optional<SerialFrame<StatusData>> DataParser::binary_to_struct(std::span<const uint8_t> data) {
    constexpr std::size_t expected_size = sizeof(StatusData) + 6;

    if (data.size() != expected_size) {
        spdlog::error("[PARSER] The size does not match");
        return std::nullopt;
    }

    uint8_t header = data[0];
    uint8_t stop_byte = data[expected_size - 1];
    if (header != 0xAA || stop_byte != 0x55) {
        spdlog::error("[PARSER] 0xAA or 0x55");
        return std::nullopt;
    }


    std::size_t payload_offset = 4 + sizeof(StatusData);
    uint8_t received_crc = data[payload_offset];
    uint8_t calculated_crc = calculate_crc(data.data(), payload_offset);
    if (received_crc != calculated_crc) {
        spdlog::error("[PARSER] CRC does not match.");
        return std::nullopt;
        return std::nullopt;
    }
    SerialFrame<StatusData> frame;
    frame.header = header;
    frame.device_id = data[1];
    frame.cmd_id = data[2];
    frame.length = data[3];
    frame.crc8 = received_crc;
    frame.stopByte = stop_byte;

    std::memcpy(&frame.data, data.data() + 4, sizeof(StatusData));
    return frame;
}

uint8_t DataParser::calculate_crc(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}
