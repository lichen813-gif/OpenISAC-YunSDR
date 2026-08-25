#include "openisac/mimo_nxn.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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
    const std::size_t receive_ports = channel.receive_ports == 0u
        ? streams : channel.receive_ports;
    if (streams < 1u || streams > maximum_spatial_streams) {
        throw std::invalid_argument("MIMO stream count must be in [1,8]");
    }
    if (receive_ports < streams || receive_ports > maximum_spatial_streams) {
        throw std::invalid_argument(
            "MIMO receive-port count must be in [streams,8]");
    }
    if (!std::isfinite(noise_variance) || noise_variance < 0.0f) {
        throw std::invalid_argument("noise variance must be finite and non-negative");
    }
    const float scale = 1.0f / std::sqrt(static_cast<float>(streams));
    Matrix gram{};
    Vector matched{};
    for (std::size_t column = 0u; column < streams; ++column) {
        for (std::size_t row = 0u; row < receive_ports; ++row) {
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

float condition_number_nxn(const ChannelNxN& channel) {
    const std::size_t streams = channel.streams;
    const std::size_t receive_ports = channel.receive_ports == 0u
        ? streams : channel.receive_ports;
    if (streams < 1u || streams > maximum_spatial_streams) {
        throw std::invalid_argument("MIMO condition stream count must be in [1,8]");
    }
    if (receive_ports < streams || receive_ports > maximum_spatial_streams) {
        throw std::invalid_argument(
            "MIMO condition receive-port count must be in [streams,8]");
    }
    constexpr std::size_t real_dimension = 2u * maximum_spatial_streams;
    std::array<double, real_dimension * real_dimension> matrix{};
    const auto index = [=](std::size_t row, std::size_t column) {
        return row * real_dimension + column;
    };
    for (std::size_t left = 0u; left < streams; ++left) {
        for (std::size_t right = 0u; right < streams; ++right) {
            std::complex<double> gram{};
            for (std::size_t rx = 0u; rx < receive_ports; ++rx) {
                gram += std::conj(static_cast<std::complex<double>>(
                            channel.values[rx * nmax + left])) *
                        static_cast<std::complex<double>>(
                            channel.values[rx * nmax + right]);
            }
            matrix[index(left, right)] = gram.real();
            matrix[index(left, streams + right)] = -gram.imag();
            matrix[index(streams + left, right)] = gram.imag();
            matrix[index(streams + left, streams + right)] = gram.real();
        }
    }

    const std::size_t dimension = 2u * streams;
    for (std::size_t iteration = 0u;
         iteration < 16u * dimension * dimension; ++iteration) {
        std::size_t p = 0u;
        std::size_t q = 1u;
        double largest = 0.0;
        double diagonal_scale = 0.0;
        for (std::size_t row = 0u; row < dimension; ++row) {
            diagonal_scale = std::max(
                diagonal_scale, std::abs(matrix[index(row, row)]));
            for (std::size_t column = row + 1u;
                 column < dimension; ++column) {
                const double value = std::abs(matrix[index(row, column)]);
                if (value > largest) {
                    largest = value;
                    p = row;
                    q = column;
                }
            }
        }
        if (largest <= std::max(1.0e-14, diagonal_scale * 1.0e-12)) {
            break;
        }
        const double app = matrix[index(p, p)];
        const double aqq = matrix[index(q, q)];
        const double apq = matrix[index(p, q)];
        const double tau = (aqq - app) / (2.0 * apq);
        const double t = std::copysign(
            1.0 / (std::abs(tau) + std::sqrt(1.0 + tau * tau)), tau);
        const double cosine = 1.0 / std::sqrt(1.0 + t * t);
        const double sine = t * cosine;
        for (std::size_t k = 0u; k < dimension; ++k) {
            if (k == p || k == q) {
                continue;
            }
            const double akp = matrix[index(k, p)];
            const double akq = matrix[index(k, q)];
            const double rotated_p = cosine * akp - sine * akq;
            const double rotated_q = sine * akp + cosine * akq;
            matrix[index(k, p)] = rotated_p;
            matrix[index(p, k)] = rotated_p;
            matrix[index(k, q)] = rotated_q;
            matrix[index(q, k)] = rotated_q;
        }
        matrix[index(p, p)] = app - t * apq;
        matrix[index(q, q)] = aqq + t * apq;
        matrix[index(p, q)] = 0.0;
        matrix[index(q, p)] = 0.0;
    }

    double minimum = std::numeric_limits<double>::infinity();
    double maximum = 0.0;
    for (std::size_t diagonal = 0u; diagonal < dimension; ++diagonal) {
        const double value = std::max(0.0, matrix[index(diagonal, diagonal)]);
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    if (!(maximum > 0.0) || minimum <= maximum * 1.0e-12) {
        return std::numeric_limits<float>::infinity();
    }
    return static_cast<float>(std::sqrt(maximum / minimum));
}

std::complex<float> fixed_dft_precoder_4x2(
    std::size_t transmit_port,
    std::size_t layer) {
    if (transmit_port >= 4u || layer >= 2u) {
        throw std::invalid_argument("4x2 precoder index is out of range");
    }
    constexpr float half = 0.5f;
    static const std::array<std::complex<float>, 8> coefficients{{
        {half, 0.0f}, {half, 0.0f},
        {half, 0.0f}, {0.0f, half},
        {half, 0.0f}, {-half, 0.0f},
        {half, 0.0f}, {0.0f, -half},
    }};
    return coefficients[transmit_port * 2u + layer];
}

ChannelNxN apply_fixed_dft_precoder_4x2(
    const ChannelNxN& physical_channel) {
    const std::size_t physical_receive_ports =
        physical_channel.receive_ports == 0u
        ? physical_channel.streams : physical_channel.receive_ports;
    if (physical_channel.streams != 4u || physical_receive_ports != 4u) {
        throw std::invalid_argument("4x2 precoding requires a 4x4 physical channel");
    }
    ChannelNxN effective;
    effective.streams = 2u;
    effective.receive_ports = 4u;
    for (std::size_t rx = 0u; rx < 4u; ++rx) {
        for (std::size_t layer = 0u; layer < 2u; ++layer) {
            for (std::size_t tx = 0u; tx < 4u; ++tx) {
                effective.values[rx * maximum_spatial_streams + layer] +=
                    physical_channel.values[
                        rx * maximum_spatial_streams + tx] *
                    fixed_dft_precoder_4x2(tx, layer);
            }
        }
    }
    return effective;
}

}  // namespace openisac
