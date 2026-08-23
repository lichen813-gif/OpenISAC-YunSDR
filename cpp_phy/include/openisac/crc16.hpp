#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace openisac {

std::uint16_t crc16_ccitt_false(const std::uint8_t* data, std::size_t size);
std::uint16_t crc16_ccitt_false(const std::vector<std::uint8_t>& data);
bool check_crc16_ccitt_false(const std::vector<std::uint8_t>& frame);

}  // namespace openisac
