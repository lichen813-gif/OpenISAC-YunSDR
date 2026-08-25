#pragma once

#include "openisac/mimo2x2.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace openisac {

enum class Modulation { qpsk, qam16, qam64, qam256 };

enum class TransmissionScheme {
    spatial_multiplexing,
    alamouti_stbc,
};

unsigned modulation_bits(Modulation modulation) noexcept;
const char* modulation_name(Modulation modulation) noexcept;
const char* transmission_scheme_name(TransmissionScheme scheme) noexcept;

struct LinkMode {
    unsigned rank = 1u;
    Modulation modulation = Modulation::qpsk;
    TransmissionScheme scheme = TransmissionScheme::spatial_multiplexing;
    // Zero selects the legacy default: four ports for Rank-4, otherwise two.
    // Explicit four enables the 4Tx/4Rx Rank-2 precoded profile.
    unsigned transmit_ports = 0u;
};

unsigned physical_transmit_ports(const LinkMode& mode) noexcept;

bool operator==(const LinkMode& left, const LinkMode& right) noexcept;
bool operator!=(const LinkMode& left, const LinkMode& right) noexcept;

struct LinkDecision {
    LinkMode desired{};
    bool outage = false;
    bool configured_mcs_supported = false;
    float rank2_bottleneck_sinr_db = 0.0f;
    float rank1_sinr_db = 0.0f;
    float minimum_eigenvalue_ratio = 0.0f;
};

LinkDecision recommend_rank_mcs(
    const std::vector<Channel2x2>& channels,
    const std::vector<std::array<float, 2>>& equivalent_mse,
    float noise_variance,
    LinearDetector detector,
    Modulation configured_modulation,
    float rank2_minimum_eigenvalue_ratio = 0.01f,
    float rank2_minimum_sinr_db = 4.0f,
    float implementation_margin_db = 2.0f);

enum class ControllerReason {
    hold,
    quality_downshift,
    crc_fast_downshift,
    outage_fast_downshift,
    upshift_hysteresis,
    confirmed_step_upshift,
};

struct ControllerUpdate {
    LinkMode previous{};
    LinkMode desired{};
    LinkMode selected{};
    ControllerReason reason = ControllerReason::hold;
    unsigned pending_upshift_count = 0u;
};

class AdaptiveLinkController {
public:
    explicit AdaptiveLinkController(
        LinkMode initial_mode = {2u, Modulation::qam64},
        unsigned upshift_confirmation_frames = 3u);

    const LinkMode& current() const noexcept { return current_; }
    ControllerUpdate observe(
        LinkMode desired, bool crc_failed = false, bool outage = false);

private:
    LinkMode current_{};
    unsigned confirmation_frames_ = 3u;
    LinkMode pending_{};
    unsigned pending_count_ = 0u;
    bool pending_valid_ = false;

    void reset_pending() noexcept;
};

}  // namespace openisac
