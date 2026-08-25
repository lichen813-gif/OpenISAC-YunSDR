#pragma once

#include "libyunsdr_isac/transceiver.hpp"

#include <memory>

namespace libyunsdr_isac {

// Keeps the vendor C API private to the implementation translation unit.
std::unique_ptr<IVendorTransport> make_libyunsdr_transport();

}  // namespace libyunsdr_isac
