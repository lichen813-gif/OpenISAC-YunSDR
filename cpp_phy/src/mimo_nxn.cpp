#include "openisac/mimo_nxn.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openisac {
namespace {

using Complex = std::complex<float>;
constexpr std::size_t nmax = maximum_spatial_streams;
using Matrix = std::array<Complex, nmax * nmax>;
using Vector = std::array<Complex, nmax>;

Complex& at(Matrix& matrix, std::size_t row, std::size_t column) {
    return matrix[row * nmax + column];
}

const Complex& at(const Matrix& matrix, std::size_t row, std::size_t column) {
    return matrix[row * nmax + column];
}

Matrix cholesky(const Matrix& matrix, std::size_t streams) {
    Matrix lower{};
    for (std::size_t row = 0u; row < streams; ++row) {
        for (std::size_t column = 0u; column <= row; ++column) {
            Complex value = at(matrix, row, column);
            for (std::size_t inner = 0u; inner < column; ++inner) {
                value -= at(lower, row, inner) *
                         std::conj(at(lower, column, inner));
            }
            if (row == column) {
                const float diagonal = value.real();
                if (!std::isfinite(diagonal) || diagonal <= 1.0e-10f) {
                    throw std::runtime_error(
                        "singular or non-positive MIMO Gram matrix");
                }
                at(lower, row, column) = {std::sqrt(diagonal), 0.0f};
            } else {
                at(lower, row, column) =
                    value / at(lower, column, column).real();
            }
        }
    }
    return lower;
}

Vector solve_cholesky(
    const Matrix& lower,
    const Vector& right,
    std::size_t streams) {
    Vector intermediate{};
    for (std::size_t row = 0u; row < streams; ++row) {
        Complex value = right[row];
        for (std::size_t column = 0u; column < row; ++column) {
            value -= at(lower, row, column) * intermediate[column];
        }
        intermediate[row] = value / at(lower, row, row).real();
    }
    Vector result{};
    for (std::size_t reverse = 0u; reverse < streams; ++reverse) {
        const std::size_t row = streams - 1u - reverse;
        Complex value = intermediate[row];
        for (std::size_t column = row + 1u; column < streams; ++column) {
            value -= std::conj(at(lower, column, row)) * result[column];
        }
        result[row] = value / at(lower, row, row).real();
    }
    return result;
}

}  // namespace

DetectionNxN detect_nxn(
    const std::array<std::complex<float>, maximum_spatial_streams>& received,
    const ChannelNxN& channel,
    float noise_variance,
    LinearDetector detector) {
    const std::size_t streams = channel.streams;
    if (streams < 1u || streams > maximum_spatial_streams) {
        throw std::invalid_argument("MIMO stream count must be in [1,8]");
    }
    if (!std::isfinite(noise_variance) || noise_variance < 0.0f) {
        throw std::invalid_argument("noise variance must be finite and non-negative");
    }
    const float scale = 1.0f / std::sqrt(static_cast<float>(streams));
    Matrix gram{};
    Vector matched{};
    for (std::size_t column = 0u; column < streams; ++column) {
        for (std::size_t row = 0u; row < streams; ++row) {
            const Complex h = channel.values[row * nmax + column] * scale;
            matched[column] += std::conj(h) * received[row];
            for (std::size_t other = 0u; other < streams; ++other) {
                const Complex h_other =
                    channel.values[row * nmax + other] * scale;
                at(gram, column, other) += std::conj(h) * h_other;
            }
        }
    }
    if (detector == LinearDetector::mmse) {
        for (std::size_t stream = 0u; stream < streams; ++stream) {
            at(gram, stream, stream) += noise_variance;
        }
    }
    const Matrix lower = cholesky(gram, streams);
    const Vector symbols = solve_cholesky(lower, matched, streams);
    DetectionNxN result;
    result.streams = streams;
    result.symbols = symbols;
    if (noise_variance > 0.0f) {
        for (std::size_t stream = 0u; stream < streams; ++stream) {
            Vector unit{};
            unit[stream] = {1.0f, 0.0f};
            const Vector inverse_column =
                solve_cholesky(lower, unit, streams);
            result.predicted_mse[stream] = std::max(
                0.0f, noise_variance * inverse_column[stream].real());
        }
    }
    return result;
}

}  // namespace openisac
