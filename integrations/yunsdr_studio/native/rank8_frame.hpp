#pragma once

#include "openisac/channel_estimation.hpp"
#include "openisac/dynamic_frame.hpp"

namespace studio_rank8 {

openisac::FormalFrameLayout layout(openisac::Modulation modulation, unsigned edge = 448);
void pilots(bool dmrs, std::vector<std::complex<float>>& grid, unsigned edge = 448);
void estimate_dmrs(const std::vector<std::complex<float>>& received,
                   const std::vector<std::complex<float>>& reference,
                   const openisac::FormalFrameLayout& shape,
                   std::vector<openisac::ChannelNxN>& channels);
openisac::MiniHeader header(const std::vector<float>& llrs, bool dmrs);
openisac::EncodedDynamicFrame encode(const std::vector<std::uint8_t>& payload,
                                     openisac::LinkMode mode, std::uint16_t sequence, bool dmrs,
                                     const openisac::Ldpc5041008& codec);
openisac::DecodedDynamicFrame decode_payload(const openisac::MiniHeader& control,
                                     openisac::LinkMode mode,
                                     const openisac::FormalFrameLayout& shape,
                                     const std::vector<std::complex<float>>& equalized,
                                     const std::vector<float>& variance,
                                     const openisac::Ldpc5041008& codec);

}  // namespace studio_rank8
