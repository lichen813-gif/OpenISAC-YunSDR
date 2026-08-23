#include "openisac/crc16.hpp"

namespace openisac {

std::uint16_t crc16_ccitt_false(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xFFFFu;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint16_t>(data[index]) << 8u;
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (crc & 0x8000u) != 0u
                      ? static_cast<std::uint16_t>((crc << 1u) ^ 0x1021u)
                      : static_cast<std::uint16_t>(crc << 1u);
        }
    }
    return crc;
}

std::uint16_t crc16_ccitt_false(const std::vector<std::uint8_t>& data) {
    return crc16_ccitt_false(data.data(), data.size());
}

bool check_crc16_ccitt_false(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 2u) {
        return false;
    }
    const std::size_t payload_size = frame.size() - 2u;
    const std::uint16_t expected = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(frame[payload_size]) << 8u) |
        frame[payload_size + 1u]);
    return crc16_ccitt_false(frame.data(), payload_size) == expected;
}

}  // namespace openisac
