#include "libyunsdr_isac/backend.hpp"

#include <stdexcept>

namespace libyunsdr_isac {

bool hardware_backend_available() noexcept {
    return false;
}

radio::IDevicePtr make_device(const DeviceSettings&) {
    throw std::runtime_error(
        "libyunsdr hardware backend is not built; import and verify the target "
        "Windows SDK before enabling hardware access");
}

}  // namespace libyunsdr_isac
