#include "openisac/tdl_channel.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openisac {
namespace {

void validate_taps(const std::vector<TdlTap>& taps) {
    if (taps.empty()) {
        throw std::invalid_argument("TDL channel requires at least one tap");
    }
    for (const auto& tap : taps) {
        if (!std::isfinite(tap.gain_db) || !std::isfinite(tap.phase_degrees) ||
            !std::isfinite(tap.doppler_hz)) {
            throw std::invalid_argument("TDL tap gain, phase and Doppler must be finite");
        }
    }
}

std::uint32_t next_random(std::uint32_t& state) noexcept {
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return state;
}

float uniform_open(std::uint32_t& state) noexcept {
    return (static_cast<float>(next_random(state)) + 0.5f) /
        4294967296.0f;
}

std::complex<float> gaussian_complex(std::uint32_t& state) {
    constexpr float pi = 3.14159265358979323846f;
    const float radius = std::sqrt(-2.0f * std::log(uniform_open(state)));
    const float angle = 2.0f * pi * uniform_open(state);
    constexpr float inverse_sqrt_two = 0.7071067811865475244f;
    return inverse_sqrt_two * radius *
        std::complex<float>{std::cos(angle), std::sin(angle)};
}

std::array<std::array<float, 2>, 2> correlation_square_root(float rho) {
    if (!std::isfinite(rho) || std::abs(rho) >= 1.0f) {
        throw std::invalid_argument("TDL spatial correlation magnitude must be below one");
    }
    const float positive = std::sqrt(1.0f + rho);
    const float negative = std::sqrt(1.0f - rho);
    const float diagonal = 0.5f * (positive + negative);
    const float off_diagonal = 0.5f * (positive - negative);
    return {{{diagonal, off_diagonal}, {off_diagonal, diagonal}}};
}

void normalize_average_link_power(ImpulseResponse2x2& response) {
    double total_power = 0.0;
    for (const auto& receive : response) {
        for (const auto& link : receive) {
            for (const auto& value : link) {
                total_power += std::norm(value);
            }
        }
    }
    if (!(total_power > 0.0) || !std::isfinite(total_power)) {
        throw std::invalid_argument("TDL channel has zero or invalid power");
    }
    const float scale = std::sqrt(4.0f / static_cast<float>(total_power));
    for (auto& receive : response) {
        for (auto& link : receive) {
            for (auto& value : link) {
                value *= scale;
            }
        }
    }
}

using RealMatrixNxN = std::array<
    float, maximum_spatial_streams * maximum_spatial_streams>;

RealMatrixNxN exponential_correlation_cholesky(
    std::size_t streams,
    float rho) {
    if (streams == 0u || streams > maximum_spatial_streams ||
        !std::isfinite(rho) || std::abs(rho) >= 1.0f) {
        throw std::invalid_argument(
            "generic TDL dimensions or spatial correlation are invalid");
    }
    RealMatrixNxN lower{};
    for (std::size_t row = 0u; row < streams; ++row) {
        for (std::size_t column = 0u; column <= row; ++column) {
            const std::size_t separation = row > column
                ? row - column : column - row;
            float value = 1.0f;
            for (std::size_t step = 0u; step < separation; ++step) {
                value *= rho;
            }
            for (std::size_t inner = 0u; inner < column; ++inner) {
                value -= lower[row * maximum_spatial_streams + inner] *
                         lower[column * maximum_spatial_streams + inner];
            }
            if (row == column) {
                if (!(value > 0.0f) || !std::isfinite(value)) {
                    throw std::invalid_argument(
                        "generic TDL correlation matrix is not positive definite");
                }
                lower[row * maximum_spatial_streams + column] = std::sqrt(value);
            } else {
                lower[row * maximum_spatial_streams + column] =
                    value /
                    lower[column * maximum_spatial_streams + column];
            }
        }
    }
    return lower;
}

void normalize_average_link_power(ImpulseResponseNxN& response) {
    double total_power = 0.0;
    for (std::size_t rx = 0u; rx < response.streams; ++rx) {
        for (std::size_t tx = 0u; tx < response.streams; ++tx) {
            const auto& link = response.links[
                rx * maximum_spatial_streams + tx];
            for (const auto& value : link) {
                total_power += std::norm(value);
            }
        }
    }
    if (!(total_power > 0.0) || !std::isfinite(total_power)) {
        throw std::invalid_argument("generic TDL channel has zero or invalid power");
    }
    const float links = static_cast<float>(
        response.streams * response.streams);
    const float scale = std::sqrt(links / static_cast<float>(total_power));
    for (std::size_t rx = 0u; rx < response.streams; ++rx) {
        for (std::size_t tx = 0u; tx < response.streams; ++tx) {
            auto& link = response.links[
                rx * maximum_spatial_streams + tx];
            for (auto& value : link) {
                value *= scale;
            }
        }
    }
}

}  // namespace

ImpulseResponse2x2 build_deterministic_tdl_2x2(const std::vector<TdlTap>& taps) {
    validate_taps(taps);
    std::size_t maximum_delay = 0u;
    for (const auto& tap : taps) {
        maximum_delay = std::max(maximum_delay, tap.delay_samples);
    }
    ImpulseResponse2x2 response;
    for (auto& receive : response) {
        for (auto& link : receive) {
            link.assign(maximum_delay + 1u, {});
        }
    }
    constexpr float pi = 3.14159265358979323846f;
    for (std::size_t tap_index = 0u; tap_index < taps.size(); ++tap_index) {
        const auto& tap = taps[tap_index];
        const float amplitude = std::pow(10.0f, tap.gain_db / 20.0f);
        for (std::size_t rx = 0u; rx < 2u; ++rx) {
            for (std::size_t tx = 0u; tx < 2u; ++tx) {
                const float spatial_phase =
                    (static_cast<float>(rx) * 0.37f - static_cast<float>(tx) * 0.61f +
                     static_cast<float>(rx * tx) * 0.19f) *
                    static_cast<float>(tap_index + 1u);
                const float phase = tap.phase_degrees * pi / 180.0f + spatial_phase;
                response[rx][tx][tap.delay_samples] =
                    amplitude * std::complex<float>{std::cos(phase), std::sin(phase)};
            }
        }
    }
    for (auto& receive : response) {
        for (auto& link : receive) {
            double power = 0.0;
            for (const auto& value : link) {
                power += std::norm(value);
            }
            const float scale = 1.0f / std::sqrt(static_cast<float>(power));
            for (auto& value : link) {
                value *= scale;
            }
        }
    }
    return response;
}

ImpulseResponse2x2 build_correlated_tdl_2x2(
    const std::vector<TdlTap>& taps,
    const TdlSpatialCorrelationConfig& correlation) {
    validate_taps(taps);
    const auto transmit_root = correlation_square_root(
        correlation.transmit_correlation);
    const auto receive_root = correlation_square_root(
        correlation.receive_correlation);
    std::size_t maximum_delay = 0u;
    for (const auto& tap : taps) {
        maximum_delay = std::max(maximum_delay, tap.delay_samples);
    }
    ImpulseResponse2x2 response;
    for (auto& receive : response) {
        for (auto& link : receive) {
            link.assign(maximum_delay + 1u, {});
        }
    }

    std::uint32_t random_state = correlation.random_seed == 0u
        ? 0xA341316Cu : correlation.random_seed;
    constexpr float pi = 3.14159265358979323846f;
    for (const auto& tap : taps) {
        std::array<std::array<std::complex<float>, 2>, 2> independent{};
        for (auto& receive : independent) {
            for (auto& value : receive) {
                value = gaussian_complex(random_state);
            }
        }
        std::array<std::array<std::complex<float>, 2>, 2> receive_correlated{};
        std::array<std::array<std::complex<float>, 2>, 2> correlated{};
        for (std::size_t rx = 0u; rx < 2u; ++rx) {
            for (std::size_t tx = 0u; tx < 2u; ++tx) {
                for (std::size_t source = 0u; source < 2u; ++source) {
                    receive_correlated[rx][tx] +=
                        receive_root[rx][source] * independent[source][tx];
                }
            }
        }
        for (std::size_t rx = 0u; rx < 2u; ++rx) {
            for (std::size_t tx = 0u; tx < 2u; ++tx) {
                for (std::size_t source = 0u; source < 2u; ++source) {
                    correlated[rx][tx] +=
                        receive_correlated[rx][source] * transmit_root[source][tx];
                }
            }
        }
        const float amplitude = std::pow(10.0f, tap.gain_db / 20.0f);
        const float phase = tap.phase_degrees * pi / 180.0f;
        const std::complex<float> path = amplitude *
            std::complex<float>{std::cos(phase), std::sin(phase)};
        for (std::size_t rx = 0u; rx < 2u; ++rx) {
            for (std::size_t tx = 0u; tx < 2u; ++tx) {
                response[rx][tx][tap.delay_samples] += path * correlated[rx][tx];
            }
        }
    }
    normalize_average_link_power(response);
    return response;
}

ImpulseResponseNxN build_correlated_tdl_nxn(
    const std::vector<TdlTap>& taps,
    const TdlSpatialCorrelationNxNConfig& correlation) {
    validate_taps(taps);
    const std::size_t streams = correlation.streams;
    const auto transmit_root = exponential_correlation_cholesky(
        streams, correlation.transmit_correlation);
    const auto receive_root = exponential_correlation_cholesky(
        streams, correlation.receive_correlation);
    std::size_t maximum_delay = 0u;
    for (const auto& tap : taps) {
        maximum_delay = std::max(maximum_delay, tap.delay_samples);
    }
    ImpulseResponseNxN response;
    response.streams = streams;
    for (std::size_t rx = 0u; rx < streams; ++rx) {
        for (std::size_t tx = 0u; tx < streams; ++tx) {
            response.links[rx * maximum_spatial_streams + tx].assign(
                maximum_delay + 1u, {});
        }
    }

    using ComplexMatrix = std::array<
        std::complex<float>,
        maximum_spatial_streams * maximum_spatial_streams>;
    std::uint32_t random_state = correlation.random_seed == 0u
        ? 0xA341316Cu : correlation.random_seed;
    constexpr float pi = 3.14159265358979323846f;
    for (const auto& tap : taps) {
        ComplexMatrix independent{};
        ComplexMatrix receive_correlated{};
        ComplexMatrix correlated{};
        for (std::size_t rx = 0u; rx < streams; ++rx) {
            for (std::size_t tx = 0u; tx < streams; ++tx) {
                independent[rx * maximum_spatial_streams + tx] =
                    gaussian_complex(random_state);
            }
        }
        for (std::size_t rx = 0u; rx < streams; ++rx) {
            for (std::size_t tx = 0u; tx < streams; ++tx) {
                for (std::size_t source = 0u; source < streams; ++source) {
                    receive_correlated[rx * maximum_spatial_streams + tx] +=
                        receive_root[rx * maximum_spatial_streams + source] *
                        independent[source * maximum_spatial_streams + tx];
                }
            }
        }
        for (std::size_t rx = 0u; rx < streams; ++rx) {
            for (std::size_t tx = 0u; tx < streams; ++tx) {
                for (std::size_t source = 0u; source < streams; ++source) {
                    correlated[rx * maximum_spatial_streams + tx] +=
                        receive_correlated[
                            rx * maximum_spatial_streams + source] *
                        transmit_root[
                            tx * maximum_spatial_streams + source];
                }
            }
        }
        const float amplitude = std::pow(10.0f, tap.gain_db / 20.0f);
        const float phase = tap.phase_degrees * pi / 180.0f;
        const std::complex<float> path = amplitude *
            std::complex<float>{std::cos(phase), std::sin(phase)};
        for (std::size_t rx = 0u; rx < streams; ++rx) {
            for (std::size_t tx = 0u; tx < streams; ++tx) {
                response.links[rx * maximum_spatial_streams + tx]
                    [tap.delay_samples] += path *
                    correlated[rx * maximum_spatial_streams + tx];
            }
        }
    }
    normalize_average_link_power(response);
    return response;
}

std::vector<TdlTap> evaluate_tdl_taps(
    const std::vector<TdlTap>& taps,
    double time_seconds) {
    if (!std::isfinite(time_seconds)) {
        throw std::invalid_argument("TDL evaluation time must be finite");
    }
    std::vector<TdlTap> result = taps;
    for (auto& tap : result) {
        if (!std::isfinite(tap.doppler_hz)) {
            throw std::invalid_argument("TDL Doppler must be finite");
        }
        const double phase = static_cast<double>(tap.phase_degrees) +
            360.0 * static_cast<double>(tap.doppler_hz) * time_seconds;
        tap.phase_degrees = static_cast<float>(std::remainder(phase, 360.0));
    }
    return result;
}

std::array<std::vector<std::complex<float>>, 2> apply_tdl_2x2_symbol(
    const std::array<std::vector<std::complex<float>>, 2>& transmitted,
    const ImpulseResponse2x2& impulse_response) {
    std::array<std::vector<std::complex<float>>, 2> received;
    apply_tdl_2x2_symbol(transmitted, impulse_response, received);
    return received;
}

void apply_tdl_2x2_symbol(
    const std::array<std::vector<std::complex<float>>, 2>& transmitted,
    const ImpulseResponse2x2& impulse_response,
    std::array<std::vector<std::complex<float>>, 2>& received) {
    if (transmitted[0].size() != transmitted[1].size()) {
        throw std::invalid_argument("TDL transmit branches must have equal length");
    }
    for (auto& branch : received) {
        branch.assign(transmitted[0].size(), {});
    }
    for (std::size_t rx = 0u; rx < 2u; ++rx) {
        for (std::size_t tx = 0u; tx < 2u; ++tx) {
            const auto& response = impulse_response[rx][tx];
            for (std::size_t sample = 0u; sample < transmitted[tx].size(); ++sample) {
                for (std::size_t delay = 0u; delay < response.size() && delay <= sample; ++delay) {
                    received[rx][sample] +=
                        transmitted[tx][sample - delay] * response[delay];
                }
            }
        }
    }
}

Channel2x2 tdl_frequency_response(
    const ImpulseResponse2x2& impulse_response,
    std::size_t fft_index,
    std::size_t fft_size) {
    if (fft_size == 0u || fft_index >= fft_size) {
        throw std::invalid_argument("invalid TDL FFT coordinate");
    }
    constexpr float pi = 3.14159265358979323846f;
    std::array<std::complex<float>, 4> links{};
    for (std::size_t rx = 0u; rx < 2u; ++rx) {
        for (std::size_t tx = 0u; tx < 2u; ++tx) {
            for (std::size_t delay = 0u; delay < impulse_response[rx][tx].size(); ++delay) {
                const float phase = -2.0f * pi * static_cast<float>(fft_index * delay) /
                                    static_cast<float>(fft_size);
                links[rx * 2u + tx] += impulse_response[rx][tx][delay] *
                                       std::complex<float>{std::cos(phase), std::sin(phase)};
            }
        }
    }
    return {links[0], links[1], links[2], links[3]};
}

void apply_tdl_nxn_symbol(
    const std::array<
        std::vector<std::complex<float>>, maximum_spatial_streams>& transmitted,
    const ImpulseResponseNxN& impulse_response,
    std::array<
        std::vector<std::complex<float>>, maximum_spatial_streams>& received) {
    const std::size_t streams = impulse_response.streams;
    if (streams == 0u || streams > maximum_spatial_streams) {
        throw std::invalid_argument("generic TDL stream count must be in [1,8]");
    }
    const std::size_t samples = transmitted[0].size();
    for (std::size_t tx = 0u; tx < streams; ++tx) {
        if (transmitted[tx].size() != samples) {
            throw std::invalid_argument(
                "generic TDL transmit branches must have equal length");
        }
    }
    for (std::size_t rx = 0u; rx < streams; ++rx) {
        received[rx].assign(samples, {});
    }
    for (std::size_t rx = 0u; rx < streams; ++rx) {
        for (std::size_t tx = 0u; tx < streams; ++tx) {
            const auto& response = impulse_response.links[
                rx * maximum_spatial_streams + tx];
            for (std::size_t sample = 0u; sample < samples; ++sample) {
                for (std::size_t delay = 0u;
                     delay < response.size() && delay <= sample; ++delay) {
                    received[rx][sample] += transmitted[tx][sample - delay] *
                                            response[delay];
                }
            }
        }
    }
}

ChannelNxN tdl_frequency_response_nxn(
    const ImpulseResponseNxN& impulse_response,
    std::size_t fft_index,
    std::size_t fft_size) {
    if (impulse_response.streams == 0u ||
        impulse_response.streams > maximum_spatial_streams ||
        fft_size == 0u || fft_index >= fft_size) {
        throw std::invalid_argument("invalid generic TDL FFT coordinate");
    }
    ChannelNxN result;
    result.streams = impulse_response.streams;
    constexpr float pi = 3.14159265358979323846f;
    for (std::size_t rx = 0u; rx < result.streams; ++rx) {
        for (std::size_t tx = 0u; tx < result.streams; ++tx) {
            const auto& link = impulse_response.links[
                rx * maximum_spatial_streams + tx];
            auto& value = result.values[
                rx * maximum_spatial_streams + tx];
            for (std::size_t delay = 0u; delay < link.size(); ++delay) {
                const float phase = -2.0f * pi *
                    static_cast<float>(fft_index * delay) /
                    static_cast<float>(fft_size);
                value += link[delay] *
                    std::complex<float>{std::cos(phase), std::sin(phase)};
            }
        }
    }
    return result;
}

}  // namespace openisac
