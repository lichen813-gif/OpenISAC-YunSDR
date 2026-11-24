#include "openisac/ofdm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

struct StudioMetrics {
    std::uint32_t sequence, crc, payload_bytes, symbols, bits, coded_symbols;
    float cfo_hz, sfo_ppm, evm[8];
    std::uint32_t bit_errors[8], compared_bits[8];
};

extern "C" {
unsigned studio_isac_abi() noexcept;
void* studio_isac_open(const char* matrices, char* error) noexcept;
void studio_isac_close(void* codec) noexcept;
int studio_isac_profile(unsigned ports, unsigned stbc, unsigned bits, unsigned edge,
                        std::uint32_t* capacity, std::uint32_t* symbols, char* error) noexcept;
int studio_isac_encode(void* codec, unsigned ports, unsigned stbc, unsigned bits,
                       unsigned dmrs, unsigned edge, unsigned sequence, const std::uint8_t* bytes,
                       std::size_t count, float* iq, float* constellation, char* error) noexcept;
int studio_isac_decode(void* codec, unsigned ports, unsigned stbc, unsigned dmrs, unsigned edge,
                       const float* iq, std::uint8_t* bytes, float* constellation, float* csi,
                       StudioMetrics* info, char* error) noexcept;
}

namespace {
constexpr unsigned fft_size = 1024, cp = 128, edge = 408;
constexpr unsigned symbol_samples = fft_size + cp;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void verify_spectrum(const std::vector<float>& iq, unsigned ports, unsigned slots) {
    const auto samples = slots * symbol_samples;
    std::vector<std::complex<float>> spectrum(fft_size);
    std::vector<std::uint8_t> occupied(fft_size);
    for (unsigned slot = 0; slot < slots; ++slot) {
        std::fill(occupied.begin(), occupied.end(), 0);
        for (unsigned tx = 0; tx < ports; ++tx) {
            const auto offset = tx*samples + slot*symbol_samples + cp;
            for (unsigned k = 0; k < fft_size; ++k)
                spectrum[k] = {iq[2*(offset+k)], iq[2*(offset+k)+1]};
            openisac::fft_inplace(spectrum, false);
            for (unsigned k = 0; k < fft_size; ++k)
                occupied[k] |= std::abs(spectrum[k]) > 0.001f;
        }
        for (unsigned k = 0; k < fft_size; ++k)
            if (k == 0 || (k > edge && k < fft_size-edge))
                require(!occupied[k], "transmit carrier outside the 816-bin allocation");
        if (slot == 0)
            require(std::count(occupied.begin(), occupied.end(), 1) == 2*edge,
                    "synchronization preamble does not use all 816 allocated carriers");
    }
}

void verify_mode(void* codec, unsigned ports, unsigned stbc, unsigned bits, unsigned dmrs) {
    char error[512]{};
    std::uint32_t capacity = 0, symbols = 0;
    require(studio_isac_profile(ports, stbc, bits, edge, &capacity, &symbols, error), error);
    std::vector<std::uint8_t> payload(std::min<std::uint32_t>(128, capacity)), received(capacity);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint8_t>((i*149+77) & 255);
    const unsigned slots = dmrs ? 5 : 3;
    std::vector<float> iq(2*ports*slots*symbol_samples), reference(2*symbols);
    require(studio_isac_encode(codec, ports, stbc, bits, dmrs, edge, 23, payload.data(),
                               payload.size(), iq.data(), reference.data(), error), error);
    verify_spectrum(iq, ports, slots);
    std::vector<float> constellation(2*symbols), csi(2*ports*ports*fft_size);
    StudioMetrics info{};
    require(studio_isac_decode(codec, ports, stbc, dmrs, edge, iq.data(), received.data(),
                               constellation.data(), csi.data(), &info, error), error);
    require(info.crc && info.sequence == 23 && info.payload_bytes == payload.size() &&
            std::equal(payload.begin(), payload.end(), received.begin()), "payload or CRC mismatch");
    const auto layers = stbc ? 1u : ports;
    for (unsigned layer = 0; layer < layers; ++layer)
        require(info.evm[layer] < 0.2f && info.compared_bits[layer] && !info.bit_errors[layer],
                "coded-symbol EVM or error count mismatch");
    for (unsigned i = 0; i < ports; ++i) {
        const auto offset = 2*(i*ports+i)*fft_size;
        require(std::hypot(csi[offset+2], csi[offset+3]) > 0.5f, "missing diagonal CSI");
        require(csi[offset] == 0.0f && csi[offset+1] == 0.0f, "nonzero DC CSI");
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    try {
        require(studio_isac_abi() == 7, "Studio adapter ABI mismatch");
        char error[512]{};
        const std::unique_ptr<void, decltype(&studio_isac_close)> codec(
            studio_isac_open(argv[1], error), studio_isac_close);
        require(static_cast<bool>(codec), error);
        constexpr std::array<std::array<unsigned, 2>, 5> modes{{{1, 0}, {2, 0}, {4, 0}, {8, 0}, {2, 1}}};
        for (const auto& mode : modes)
            for (unsigned bits : {2u, 4u, 6u, 8u})
                for (unsigned dmrs : {0u, 1u})
                    verify_mode(codec.get(), mode[0], mode[1], bits, dmrs);
        std::cout << "40 Studio 816-carrier profiles passed IQ, payload, CRC, EVM and CSI verification\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
