#include "openisac/link_adaptation.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace openisac {
namespace {

constexpr std::array<LinkMode, 8> kModes{{
    {1u, Modulation::qpsk},
    {1u, Modulation::qam16},
    {2u, Modulation::qpsk},
    {1u, Modulation::qam64},
    {2u, Modulation::qam16},
    {1u, Modulation::qam256},
    {2u, Modulation::qam64},
    {2u, Modulation::qam256},
}};

float required_sinr_db(Modulation modulation) noexcept {
    switch (modulation) {
        case Modulation::qpsk: return 4.0f;
        case Modulation::qam16: return 10.0f;
        case Modulation::qam64: return 16.0f;
        case Modulation::qam256: return 26.0f;
    }
    return 150.0f;
}

std::size_t mode_index(const LinkMode& mode) {
    const auto found = std::find(kModes.begin(), kModes.end(), mode);
    if (found == kModes.end()) {
        throw std::invalid_argument("unsupported link mode");
    }
    return static_cast<std::size_t>(std::distance(kModes.begin(), found));
}

Modulation mcs_from_sinr(float sinr_db, float margin_db) noexcept {
    const float usable = sinr_db - margin_db;
    if (usable >= 26.0f) return Modulation::qam256;
    if (usable >= 16.0f) return Modulation::qam64;
    if (usable >= 10.0f) return Modulation::qam16;
    return Modulation::qpsk;
}

LinkMode confirmed_step(LinkMode current, LinkMode desired) {
    const unsigned current_bits = modulation_bits(current.modulation);
    const unsigned desired_bits = modulation_bits(desired.modulation);
    if (desired_bits > current_bits) {
        Modulation next = Modulation::qam16;
        if (current.modulation == Modulation::qam16) next = Modulation::qam64;
        if (current.modulation == Modulation::qam64) next = Modulation::qam256;
        return {current.rank, next};
    }
    return desired;
}

}  // namespace

unsigned modulation_bits(Modulation modulation) noexcept {
    switch (modulation) {
        case Modulation::qpsk: return 2u;
        case Modulation::qam16: return 4u;
        case Modulation::qam64: return 6u;
        case Modulation::qam256: return 8u;
    }
    return 0u;
}

const char* modulation_name(Modulation modulation) noexcept {
    switch (modulation) {
        case Modulation::qpsk: return "qpsk";
        case Modulation::qam16: return "16qam";
        case Modulation::qam64: return "64qam";
        case Modulation::qam256: return "256qam";
    }
    return "unknown";
}

bool operator==(const LinkMode& left, const LinkMode& right) noexcept {
    return left.rank == right.rank && left.modulation == right.modulation;
}

bool operator!=(const LinkMode& left, const LinkMode& right) noexcept {
    return !(left == right);
}

LinkDecision recommend_rank_mcs(
    const std::vector<Channel2x2>& channels,
    const std::vector<std::array<float, 2>>& equivalent_mse,
    float noise_variance,
    LinearDetector detector,
    Modulation configured_modulation,
    float rank2_minimum_eigenvalue_ratio,
    float rank2_minimum_sinr_db,
    float implementation_margin_db) {
    if (channels.empty() || channels.size() != equivalent_mse.size()) {
        throw std::invalid_argument("channel and MSE arrays must have equal non-zero size");
    }
    if (!std::isfinite(noise_variance) || noise_variance < 0.0f) {
        throw std::invalid_argument("noise variance must be finite and non-negative");
    }
    float a = 0.0f;
    float d = 0.0f;
    std::complex<float> b{};
    std::array<float, 2> mse{};
    for (std::size_t index = 0; index < channels.size(); ++index) {
        const auto& h = channels[index];
        a += std::norm(h.h00) + std::norm(h.h10);
        d += std::norm(h.h01) + std::norm(h.h11);
        b += std::conj(h.h00) * h.h01 + std::conj(h.h10) * h.h11;
        mse[0] += equivalent_mse[index][0];
        mse[1] += equivalent_mse[index][1];
    }
    const float inverse_count = 1.0f / static_cast<float>(channels.size());
    a *= inverse_count;
    d *= inverse_count;
    b *= inverse_count;
    mse[0] = std::max(mse[0] * inverse_count, 1.0e-15f);
    mse[1] = std::max(mse[1] * inverse_count, 1.0e-15f);
    const float trace = std::max(a + d, 0.0f);
    const float root = std::sqrt(
        std::max((a - d) * (a - d) + 4.0f * std::norm(b), 0.0f));
    const float maximum = std::max(0.5f * (trace + root), 1.0e-15f);
    const float minimum = std::max(0.5f * (trace - root), 0.0f);
    std::array<float, 2> sinr{};
    for (std::size_t layer = 0; layer < 2u; ++layer) {
        sinr[layer] = detector == LinearDetector::mmse
                          ? std::max(1.0f / mse[layer] - 1.0f, 1.0e-15f)
                          : 1.0f / mse[layer];
    }
    LinkDecision result;
    result.minimum_eigenvalue_ratio = minimum / maximum;
    result.rank2_bottleneck_sinr_db =
        10.0f * std::log10(std::min(sinr[0], sinr[1]));
    result.rank1_sinr_db = noise_variance == 0.0f
                               ? 150.0f
                               : 10.0f * std::log10(std::max(
                                     trace / (2.0f * noise_variance), 1.0e-15f));
    const bool rank2_usable =
        result.minimum_eigenvalue_ratio >= rank2_minimum_eigenvalue_ratio &&
        result.rank2_bottleneck_sinr_db >=
            rank2_minimum_sinr_db + implementation_margin_db;
    const Modulation rank1_mcs =
        mcs_from_sinr(result.rank1_sinr_db, implementation_margin_db);
    const Modulation rank2_mcs =
        mcs_from_sinr(result.rank2_bottleneck_sinr_db, implementation_margin_db);
    const bool rank2 =
        rank2_usable &&
        2u * modulation_bits(rank2_mcs) > modulation_bits(rank1_mcs);
    const float selected_sinr = rank2
        ? result.rank2_bottleneck_sinr_db
        : result.rank1_sinr_db;
    result.desired = {
        rank2 ? 2u : 1u,
        rank2 ? rank2_mcs : rank1_mcs,
    };
    result.outage =
        selected_sinr - implementation_margin_db < required_sinr_db(Modulation::qpsk);
    result.configured_mcs_supported =
        rank2_usable && result.rank2_bottleneck_sinr_db - implementation_margin_db >=
                     required_sinr_db(configured_modulation);
    return result;
}

AdaptiveLinkController::AdaptiveLinkController(
    LinkMode initial_mode, unsigned upshift_confirmation_frames)
    : current_(initial_mode), confirmation_frames_(upshift_confirmation_frames) {
    mode_index(initial_mode);
    if (confirmation_frames_ == 0u) {
        throw std::invalid_argument("upshift confirmation must be positive");
    }
}

void AdaptiveLinkController::reset_pending() noexcept {
    pending_valid_ = false;
    pending_count_ = 0u;
}

ControllerUpdate AdaptiveLinkController::observe(
    LinkMode desired, bool crc_failed, bool outage) {
    const LinkMode previous = current_;
    const std::size_t current_index = mode_index(current_);
    const std::size_t desired_index = mode_index(desired);
    ControllerReason reason = ControllerReason::hold;
    if (crc_failed || outage) {
        const std::size_t selected_index =
            std::min(current_index == 0u ? 0u : current_index - 1u, desired_index);
        current_ = kModes[selected_index];
        reset_pending();
        reason = crc_failed ? ControllerReason::crc_fast_downshift
                            : ControllerReason::outage_fast_downshift;
    } else if (desired_index < current_index) {
        current_ = desired;
        reset_pending();
        reason = ControllerReason::quality_downshift;
    } else if (desired_index == current_index) {
        reset_pending();
        reason = ControllerReason::hold;
    } else {
        if (pending_valid_ && pending_ == desired) {
            ++pending_count_;
        } else {
            pending_ = desired;
            pending_valid_ = true;
            pending_count_ = 1u;
        }
        if (pending_count_ >= confirmation_frames_) {
            current_ = confirmed_step(previous, desired);
            reset_pending();
            reason = ControllerReason::confirmed_step_upshift;
        } else {
            reason = ControllerReason::upshift_hysteresis;
        }
    }
    return {previous, desired, current_, reason, pending_count_};
}

}  // namespace openisac
