#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "RadioBackend.hpp"

namespace libyunsdr_isac {

// Hardware configuration kept outside OpenISAC's simulator configuration.
// Values are defaults for the first SISO integration milestone, not claims
// about the supported range of a particular YunSDR model.
struct DeviceSettings {
    std::string uri;
    double sample_rate_hz = 15.36e6;
    double center_frequency_hz = 2.45e9;
    double bandwidth_hz = 15.36e6;
    double tx_gain_db = 0.0;
    double rx_gain_db = 0.0;
    std::vector<std::size_t> tx_channels{0};
    std::vector<std::size_t> rx_channels{0};
    bool use_external_reference = false;
};

// False in the baseline build. It becomes true only after a verified vendor
// backend is compiled and linked successfully.
bool hardware_backend_available() noexcept;

// Future factory for an implementation of radio::IDevice. The baseline stub
// throws a descriptive exception so an application cannot silently fall back
// to simulation while claiming to use hardware.
radio::IDevicePtr make_device(const DeviceSettings& settings);

}  // namespace libyunsdr_isac
