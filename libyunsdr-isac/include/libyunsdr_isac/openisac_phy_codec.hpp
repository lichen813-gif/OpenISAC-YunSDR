#pragma once

#include "libyunsdr_isac/phy_pipeline.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace libyunsdr_isac {

enum class PhyModulation {
    qpsk,
    qam16,
    qam64,
    qam256,
};

struct OpenIsacPhyCodecConfig {
    PhyMode mode = PhyMode::siso;
    PilotPattern pilot_pattern = PilotPattern::nr_dmrs;
    PhyModulation modulation = PhyModulation::qam64;
    std::string matrix_directory;
    std::size_t maximum_timing_offset_samples = 512u;
    std::size_t maximum_channel_delay_samples = 16u;
    float transmit_peak = 0.8f;
    float csi_smoothing_alpha = 1.0f;
    std::size_t ldpc_workers = 2u;
};

std::unique_ptr<IPhyFrameCodec> make_openisac_phy_codec(
    const OpenIsacPhyCodecConfig& config);

}  // namespace libyunsdr_isac
