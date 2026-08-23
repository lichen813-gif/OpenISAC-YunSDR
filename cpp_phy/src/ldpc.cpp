#include "openisac/ldpc.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace openisac {
namespace {

struct Alist {
    std::size_t columns = 0u;
    std::size_t rows = 0u;
    std::vector<std::vector<std::uint16_t>> row_adjacency;
};

std::vector<std::string> data_lines(const std::string& path) {
    std::ifstream source(path);
    if (!source) {
        throw std::runtime_error("cannot open LDPC alist: " + path);
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(source, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            lines.push_back(line.substr(first));
        }
    }
    return lines;
}

Alist read_alist(const std::string& path) {
    const auto lines = data_lines(path);
    if (lines.size() < 4u) {
        throw std::runtime_error("invalid LDPC alist: " + path);
    }
    Alist alist;
    {
        std::istringstream dimensions(lines[0]);
        dimensions >> alist.columns >> alist.rows;
    }
    if (alist.columns == 0u || alist.rows == 0u ||
        alist.columns > 65535u || alist.rows > 65535u) {
        throw std::runtime_error("invalid LDPC alist dimensions: " + path);
    }
    std::size_t line_index = 2u;
    auto read_degrees = [&](std::size_t count) {
        std::vector<std::size_t> degrees;
        degrees.reserve(count);
        while (degrees.size() < count) {
            if (line_index >= lines.size()) {
                throw std::runtime_error("truncated LDPC alist degree section: " + path);
            }
            std::istringstream values(lines[line_index++]);
            std::size_t degree = 0u;
            while (values >> degree) {
                degrees.push_back(degree);
            }
        }
        if (degrees.size() != count) {
            throw std::runtime_error("invalid LDPC alist degree count: " + path);
        }
        return degrees;
    };
    const auto column_degrees = read_degrees(alist.columns);
    const auto row_degrees = read_degrees(alist.rows);
    for (const auto degree : column_degrees) {
        if (line_index >= lines.size()) {
            throw std::runtime_error("truncated LDPC alist column adjacency: " + path);
        }
        std::istringstream values(lines[line_index++]);
        std::size_t value = 0u;
        for (std::size_t entry = 0u; entry < degree; ++entry) {
            if (!(values >> value) || value == 0u || value > alist.rows) {
                throw std::runtime_error("invalid LDPC alist column adjacency: " + path);
            }
        }
    }
    alist.row_adjacency.resize(alist.rows);
    for (std::size_t row = 0u; row < alist.rows; ++row) {
        if (line_index >= lines.size()) {
            throw std::runtime_error("truncated LDPC alist row adjacency: " + path);
        }
        std::istringstream values(lines[line_index++]);
        auto& adjacency = alist.row_adjacency[row];
        adjacency.reserve(row_degrees[row]);
        for (std::size_t entry = 0u; entry < row_degrees[row]; ++entry) {
            std::size_t value = 0u;
            if (!(values >> value) || value == 0u || value > alist.columns) {
                throw std::runtime_error("invalid LDPC alist row adjacency: " + path);
            }
            adjacency.push_back(static_cast<std::uint16_t>(value - 1u));
        }
    }
    return alist;
}

void validate_bits(const std::vector<std::uint8_t>& bits, std::size_t expected) {
    if (bits.size() != expected ||
        std::any_of(bits.begin(), bits.end(), [](std::uint8_t bit) { return bit > 1u; })) {
        throw std::invalid_argument("LDPC input has invalid dimensions or non-binary values");
    }
}

template <typename Value>
void resize_tracked(
    std::vector<Value>& values,
    std::size_t size,
    std::size_t& capacity_growths) {
    if (values.capacity() < size) {
        ++capacity_growths;
    }
    values.resize(size);
}

}  // namespace

Ldpc5041008::Ldpc5041008(
    const std::string& parity_check_alist,
    const std::string& generator_alist) {
    const auto parity = read_alist(parity_check_alist);
    const auto generator = read_alist(generator_alist);
    if (parity.columns != ldpc_encoded_bits || parity.rows != ldpc_parity_checks ||
        generator.columns != ldpc_encoded_bits ||
        generator.rows != ldpc_information_bits) {
        throw std::runtime_error("LDPC alist dimensions are not (1008,504)");
    }
    check_rows_ = parity.row_adjacency;
    generator_rows_ = generator.row_adjacency;

    std::vector<std::size_t> column_weights(ldpc_encoded_bits, 0u);
    std::vector<std::uint16_t> column_information(ldpc_encoded_bits, 0u);
    for (std::size_t information = 0u; information < generator_rows_.size(); ++information) {
        for (const auto codeword : generator_rows_[information]) {
            ++column_weights[codeword];
            column_information[codeword] = static_cast<std::uint16_t>(information);
        }
    }
    systematic_positions_.assign(ldpc_information_bits, 0u);
    std::vector<bool> found(ldpc_information_bits, false);
    for (std::size_t codeword = 0u; codeword < ldpc_encoded_bits; ++codeword) {
        if (column_weights[codeword] == 1u) {
            const auto information = column_information[codeword];
            if (found[information]) {
                throw std::runtime_error("LDPC generator is not uniquely systematic");
            }
            systematic_positions_[information] = static_cast<std::uint16_t>(codeword);
            found[information] = true;
        }
    }
    if (std::find(found.begin(), found.end(), false) != found.end()) {
        throw std::runtime_error("LDPC generator is missing systematic columns");
    }
    message_offsets_.assign(check_rows_.size() + 1u, 0u);
    for (std::size_t check = 0u; check < check_rows_.size(); ++check) {
        if (check_rows_[check].size() > 16u) {
            throw std::runtime_error("LDPC check degree exceeds decoder fixed workspace");
        }
        message_offsets_[check + 1u] =
            message_offsets_[check] + check_rows_[check].size();
    }
}

void LdpcDecodeWorkspace::release() noexcept {
    *this = LdpcDecodeWorkspace{};
}

std::vector<std::uint8_t> Ldpc5041008::encode(
    const std::vector<std::uint8_t>& information_bits) const {
    validate_bits(information_bits, ldpc_information_bits);
    std::vector<std::uint8_t> codeword(ldpc_encoded_bits, 0u);
    for (std::size_t information = 0u; information < ldpc_information_bits; ++information) {
        if (information_bits[information] != 0u) {
            for (const auto codeword_bit : generator_rows_[information]) {
                codeword[codeword_bit] ^= 1u;
            }
        }
    }
    return codeword;
}

std::size_t Ldpc5041008::syndrome_weight(
    const std::vector<std::uint8_t>& codeword_bits) const {
    validate_bits(codeword_bits, ldpc_encoded_bits);
    std::size_t weight = 0u;
    for (const auto& check : check_rows_) {
        std::uint8_t parity = 0u;
        for (const auto variable : check) {
            parity ^= codeword_bits[variable];
        }
        weight += parity;
    }
    return weight;
}

LdpcDecodeResult Ldpc5041008::decode_normalized_min_sum(
    const std::vector<float>& llrs,
    unsigned maximum_iterations,
    float normalization) const {
    LdpcDecodeWorkspace workspace;
    LdpcDecodeResult result;
    decode_normalized_min_sum(
        llrs, maximum_iterations, normalization, workspace, result);
    return result;
}

void Ldpc5041008::decode_normalized_min_sum(
    const std::vector<float>& llrs,
    unsigned maximum_iterations,
    float normalization,
    LdpcDecodeWorkspace& workspace,
    LdpcDecodeResult& result) const {
    if (llrs.size() != ldpc_encoded_bits || maximum_iterations == 0u ||
        !std::isfinite(normalization) || normalization <= 0.0f || normalization > 1.0f ||
        std::any_of(llrs.begin(), llrs.end(), [](float value) { return !std::isfinite(value); })) {
        throw std::invalid_argument("invalid LDPC min-sum decoder parameters");
    }
    resize_tracked(workspace.beliefs, llrs.size(), workspace.capacity_growths);
    std::copy(llrs.begin(), llrs.end(), workspace.beliefs.begin());
    resize_tracked(
        workspace.messages, message_offsets_.back(), workspace.capacity_growths);
    std::fill(workspace.messages.begin(), workspace.messages.end(), 0.0f);
    resize_tracked(
        result.codeword_bits, ldpc_encoded_bits, workspace.capacity_growths);
    resize_tracked(
        result.information_bits, ldpc_information_bits, workspace.capacity_growths);
    result.syndrome_weight = 0u;
    result.iterations = 0u;
    for (unsigned iteration = 1u; iteration <= maximum_iterations; ++iteration) {
        for (std::size_t check = 0u; check < check_rows_.size(); ++check) {
            const auto& variables = check_rows_[check];
            float* const previous =
                workspace.messages.data() + message_offsets_[check];
            std::array<float, 16> extrinsic{};
            float minimum = std::numeric_limits<float>::infinity();
            float second_minimum = std::numeric_limits<float>::infinity();
            std::size_t minimum_index = 0u;
            float sign_product = 1.0f;
            for (std::size_t edge = 0u; edge < variables.size(); ++edge) {
                const float value =
                    workspace.beliefs[variables[edge]] - previous[edge];
                extrinsic[edge] = value;
                const float magnitude = std::abs(value);
                if (magnitude < minimum) {
                    second_minimum = minimum;
                    minimum = magnitude;
                    minimum_index = edge;
                } else if (magnitude < second_minimum) {
                    second_minimum = magnitude;
                }
                if (value < 0.0f) {
                    sign_product = -sign_product;
                }
            }
            if (variables.size() == 1u) {
                second_minimum = minimum;
            }
            for (std::size_t edge = 0u; edge < variables.size(); ++edge) {
                const float edge_sign = extrinsic[edge] < 0.0f ? -1.0f : 1.0f;
                const float magnitude = edge == minimum_index ? second_minimum : minimum;
                const float updated = normalization * sign_product * edge_sign * magnitude;
                workspace.beliefs[variables[edge]] = extrinsic[edge] + updated;
                previous[edge] = updated;
            }
        }
        for (std::size_t bit = 0u; bit < ldpc_encoded_bits; ++bit) {
            result.codeword_bits[bit] =
                workspace.beliefs[bit] < 0.0f ? 1u : 0u;
        }
        result.iterations = iteration;
        result.syndrome_weight = syndrome_weight(result.codeword_bits);
        if (result.syndrome_weight == 0u) {
            break;
        }
    }
    for (std::size_t information = 0u; information < ldpc_information_bits; ++information) {
        result.information_bits[information] =
            result.codeword_bits[systematic_positions_[information]];
    }
}

}  // namespace openisac
