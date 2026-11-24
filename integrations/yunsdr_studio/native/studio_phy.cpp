// Raw IQ adapter for the OpenISAC formal frame. The C ABI uses explicit
// interleaved float32 I/Q pairs, independent of std::complex ABI.
#include "openisac/channel_estimation.hpp"
#include "openisac/dynamic_frame.hpp"
#include "openisac/ofdm.hpp"
#include "openisac/preamble_sync.hpp"
#include "openisac/qam.hpp"
#include "openisac/sampling_offset.hpp"
#include "openisac/sensing.hpp"
#include "rank8_frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define STUDIO_API extern "C" __declspec(dllexport)
#else
#define STUDIO_API extern "C" __attribute__((visibility("default")))
#endif

namespace {
using namespace openisac;
using Complex = std::complex<float>;
constexpr std::size_t nfft = 1024, cp = 128, symbol_size = nfft+cp;
constexpr unsigned limited_edge = 408, legacy_edge = 448;
constexpr float spacing = 15000.0f;
constexpr uint32_t pilot_seed = 0xC057u;
constexpr std::size_t antennas_max = maximum_spatial_streams;

std::vector<Complex> limited_preamble(unsigned edge) {
    auto tones = generate_zc_frequency(nfft, 29u);
    tones[0] = {};
    std::fill(tones.begin()+edge+1, tones.end()-edge, Complex{});
    return ofdm_modulate(tones, cp);
}

struct Codec {
    Ldpc5041008 ldpc;
    std::vector<Complex> preamble = generate_zc_ofdm_symbol(nfft, cp, 29u);
    std::vector<Complex> preamble_816 = limited_preamble(limited_edge);
    explicit Codec(const std::string& path)
        : ldpc(path + "/LDPC_504_1008.alist", path + "/LDPC_504_1008G.alist") {}
};

const std::vector<Complex>& preamble_for(const Codec& codec, unsigned edge) noexcept {
    if (edge == limited_edge)
        return codec.preamble_816;
    return codec.preamble;
}

struct Metrics {
    uint32_t sequence, crc, payload_bytes, symbols, bits, coded_symbols;
    float cfo_hz, sfo_ppm, evm[antennas_max];
    uint32_t bit_errors[antennas_max], compared_bits[antennas_max];
};

struct Detection {
    float range_m, doppler_hz, velocity_mps, excess_db;
};

template<class Function>
int run(char* error, Function work) noexcept {
    try {
        work();
        return 1;
    } catch (const std::exception& exc) {
        std::snprintf(error, 512, "%s", exc.what());
        return 0;
    }
}

LinkMode mode_for(unsigned ports, bool stbc, unsigned bits) {
    if ((ports != 1 && ports != 2 && ports != 4 && ports != 8) ||
        (stbc && ports != 2) || !SquareQAM::supported(bits))
        throw std::invalid_argument("unsupported OpenISAC port/rank/modulation");
    return {stbc ? 1u : ports, static_cast<Modulation>(bits/2-1),
            stbc ? TransmissionScheme::alamouti_stbc : TransmissionScheme::spatial_multiplexing, ports};
}

FormalFrameLayout frame_layout(LinkMode mode, unsigned edge) {
    if (edge != limited_edge && edge != legacy_edge)
        throw std::invalid_argument("unsupported OpenISAC carrier profile");
    if (mode.transmit_ports == 8)
        return studio_rank8::layout(mode.modulation, edge);
    FormalFrameProfile profile;
    profile.guard_left = nfft/2-edge;
    profile.guard_right = nfft/2-1-edge;
    profile.bits_per_symbol = modulation_bits(mode.modulation);
    profile.transmit_rank = mode.rank;
    profile.scheme = mode.scheme;
    if (mode.transmit_ports == 4)
        profile.pilot_spacing = 2;
    return build_formal_frame_layout(profile);
}

void fdm_pilots(const FormalFrameLayout& layout, LinkMode mode, bool narrow, std::vector<Complex>& grid) {
    const auto ports = mode.transmit_ports;
    if (!narrow) {
        if (ports == 8)
            studio_rank8::pilots(false, grid);
        else
            build_dynamic_pilot_reference_grid(pilot_seed, mode, grid);
        return;
    }
    grid.assign(2*nfft*ports, {});
    for (std::size_t i = 0; i < layout.pilot_fft_indices.size(); ++i) {
        const auto k = layout.pilot_fft_indices[i];
        const auto centered = static_cast<int>(k)-(k >= nfft/2 ? static_cast<int>(nfft) : 0);
        for (unsigned time = 0; time < 2; ++time) {
            auto mixed = static_cast<uint64_t>(pilot_seed) ^
                (static_cast<uint64_t>(centered)*0x85EBCA77ull) ^ (time*0xC2B2AE3Dull);
            mixed ^= mixed >> 16u;
            mixed *= 0x7FEB352Dull;
            mixed ^= mixed >> 15u;
            grid[(time*nfft+k)*ports+i%ports] = (mixed & 1u) == 0 ? 1.0f : -1.0f;
        }
    }
    for (const auto k : layout.phase_reference_fft_indices) {
        grid[k*ports] = 1;
        grid[(nfft+k)*ports] = 1;
    }
}

// Preserve the baseline header and LDPC encoding, then map their symbols
// to the reduced grid. STBC retains the same time-paired Alamouti mapping.
EncodedDynamicFrame encode_frame(const std::vector<uint8_t>& payload, LinkMode mode,
                                 uint16_t sequence, bool dmrs, unsigned edge, Codec& codec) {
    const auto ports = mode.transmit_ports;
    auto shape = frame_layout(mode, edge);
    if (payload.empty() || payload.size() > shape.user_payload_bytes)
        throw std::invalid_argument("OpenISAC payload exceeds selected profile capacity");
    auto frame = ports == 8 ? studio_rank8::encode(payload, mode, sequence, dmrs, codec.ldpc) :
                             encode_dynamic_frame(payload, mode, sequence, codec.ldpc, pilot_seed);
    if (edge == legacy_edge)
        return frame;
    std::vector<Complex> grid;
    fdm_pilots(shape, mode, true, grid);
    for (std::size_t i = 0; i < shape.control_data_positions.size(); ++i) {
        const auto source = frame.layout.data_fft_indices[frame.layout.control_data_positions[i]];
        const auto target = shape.data_fft_indices[shape.control_data_positions[i]];
        grid[target*ports] = frame.tx_grid[source*ports];
    }
    frame.payload_symbols.resize(shape.payload_layer_symbols);
    if (edge == limited_edge) {
        // The remaining REs must stay occupied. Vary their phases to avoid
        // producing a time-domain impulse from a run of identical QAM points.
        uint32_t state = 0x25C01A00u ^ sequence;
        const auto coded = frame.header.payload_blocks*ldpc_encoded_bits/modulation_bits(mode.modulation);
        for (std::size_t i = coded; i < frame.payload_symbols.size(); ++i) {
            state ^= state << 13u;
            state ^= state >> 17u;
            state ^= state << 5u;
            frame.payload_symbols[i] = SquareQAM::modulate(state & 3u, 2u);
        }
    }
    if (mode.scheme == TransmissionScheme::alamouti_stbc) {
        const auto pairs = shape.payload_time_indices.size()/2;
        constexpr float scale = 0.70710678118655f;
        for (std::size_t i = 0; i < pairs; ++i) {
            const auto k = shape.data_fft_indices[shape.payload_data_positions[i]];
            const auto a = frame.payload_symbols[i]*scale;
            const auto b = frame.payload_symbols[i+pairs]*scale;
            grid[k*2] = a;
            grid[k*2+1] = b;
            grid[(nfft+k)*2] = -std::conj(b);
            grid[(nfft+k)*2+1] = std::conj(a);
        }
    } else {
        const float scale = 1.0f/std::sqrt(static_cast<float>(mode.rank));
        for (std::size_t i = 0; i < shape.payload_time_indices.size(); ++i) {
            const auto k = shape.data_fft_indices[shape.payload_data_positions[i]];
            const auto offset = (shape.payload_time_indices[i]*nfft+k)*ports;
            for (unsigned layer = 0; layer < mode.rank; ++layer)
                grid[offset+layer] = frame.payload_symbols[i*mode.rank+layer]*scale;
        }
    }
    frame.layout = std::move(shape);
    frame.tx_grid = std::move(grid);
    return frame;
}

PilotMode pilot_mode(bool dmrs) { return dmrs ? PilotMode::nr_dmrs : PilotMode::fdm; }

Complex read_pair(const float* data, std::size_t i) { return {data[2*i], data[2*i+1]}; }
void write_pair(float* data, std::size_t i, Complex value) {
    data[2*i] = value.real();
    data[2*i+1] = value.imag();
}

std::vector<Complex> fft_grid(const std::vector<std::vector<Complex>>& rx, std::size_t first) {
    std::vector<Complex> grid(2*nfft*rx.size()), useful(nfft);
    for (std::size_t time = 0; time < 2; ++time) {
        for (std::size_t port = 0; port < rx.size(); ++port) {
            std::copy_n(rx[port].begin() + (first+time)*symbol_size + cp, nfft, useful.begin());
            fft_inplace(useful, false);
            for (std::size_t k = 0; k < nfft; ++k)
                grid[(time*nfft+k)*rx.size()+port] = useful[k];
        }
    }
    return grid;
}

float pilot_noise(const std::vector<Complex>& grid, const std::vector<Complex>& reference,
                  const FormalFrameLayout& layout, unsigned ports) {
    std::vector<float> powers;
    for (auto k : layout.pilot_fft_indices) {
        const auto offset = k*ports;
        const auto second = (nfft+k)*ports;
        const auto tx = std::find_if(reference.begin()+offset, reference.begin()+offset+ports,
                                     [](Complex v) { return std::norm(v) > 0; }) - (reference.begin()+offset);
        if (tx == ports)
            continue;
        const auto change = reference[second+tx] / reference[offset+tx];
        for (unsigned rx = 0; rx < ports; ++rx)
            powers.push_back(0.5f*std::norm(grid[second+rx]-grid[offset+rx]*change));
    }
    if (powers.empty())
        throw std::runtime_error("OpenISAC noise estimate has no pilot positions");
    auto middle = powers.begin()+powers.size()/2;
    std::nth_element(powers.begin(), middle, powers.end());
    return std::clamp(*middle/0.69314718f, 1e-10f, 1.0f);
}

void estimate_channels(std::vector<Complex>& grid, const std::vector<std::vector<Complex>>& rx,
                       const FormalFrameLayout& layout, LinkMode mode, bool dmrs, unsigned edge, Metrics& info,
                       std::vector<ChannelNxN>& channels, float& noise) {
    const auto ports = mode.transmit_ports;
    std::vector<Complex> reference;
    fdm_pilots(layout, mode, edge != legacy_edge, reference);
    const auto slope = estimate_sfo_phase_slope(grid, layout.phase_reference_fft_indices, nfft, ports, symbol_size);
    info.sfo_ppm = slope.coherence >= 0.8f ? slope.sfo_ppm : 0.0f;
    FdmPilotChannelEstimatorWorkspaceNxN workspace;
    if (dmrs) {
        auto pilots = fft_grid(rx, 1);
        if (slope.coherence >= 0.8f) {
            correct_symbol_phase_inplace(pilots, slope, nfft, ports, 1, 1.0f);
            correct_symbol_phase_inplace(grid, slope, nfft, ports, 0, 2.0f);
            correct_symbol_phase_inplace(grid, slope, nfft, ports, 1, 3.0f);
        }
        std::vector<Complex> dmrs_reference;
        if (ports == 8) {
            studio_rank8::pilots(true, dmrs_reference, edge);
            studio_rank8::estimate_dmrs(pilots, dmrs_reference, layout, channels);
        } else {
            build_nr_dmrs_reference_grid(nfft, layout.active_fft_indices, ports, pilot_seed, dmrs_reference);
            estimate_nr_dmrs_channel_linear_nxn(pilots, dmrs_reference, layout.active_fft_indices,
                                               2, nfft, ports, channels, workspace);
        }
        for (std::size_t time = 0; time < 2; ++time) {
            const auto residual = estimate_reference_residual_phase_slope_nxn(
                grid, reference, layout.pilot_fft_indices, channels, time, nfft, ports);
            correct_symbol_phase_inplace(grid, residual, nfft, ports, time, 1.0f);
        }
    } else {
        if (slope.coherence >= 0.8f)
            correct_second_symbol_phase_inplace(grid, slope, nfft, ports);
        estimate_fdm_pilot_channel_linear_nxn(grid, reference, layout.pilot_fft_indices,
                                            2, nfft, ports, channels, workspace);
    }
    noise = pilot_noise(grid, reference, layout, ports);
}

MiniHeader frame_header(const std::vector<Complex>& grid, const std::vector<ChannelNxN>& channels,
                        const FormalFrameLayout& layout, unsigned ports, bool dmrs, float noise, float& marker) {
    std::vector<float> llrs;
    for (auto position : layout.control_data_positions) {
        const auto k = layout.data_fft_indices[position];
        Complex y{};
        float power = 0;
        for (unsigned rx = 0; rx < ports; ++rx) {
            const auto h = channels[k].values[rx*antennas_max];
            power += std::norm(h);
            y += std::conj(h)*grid[k*ports+rx];
        }
        power = std::max(power, 1e-12f);
        const auto soft = SquareQAM::max_log_llrs(y/power, noise/power, 2);
        llrs.insert(llrs.end(), soft.begin(), soft.begin()+2);
    }
    return ports == 8 ? studio_rank8::header(llrs, dmrs) : decode_control_qpsk_llrs(llrs, &marker);
}

void payload_symbols(const std::vector<Complex>& grid, const std::vector<ChannelNxN>& channels,
                     const FormalFrameLayout& layout, LinkMode mode, float noise,
                     std::vector<Complex>& equalized, std::vector<float>& variance) {
    const auto ports = mode.transmit_ports;
    equalized.resize(layout.payload_layer_symbols);
    variance.resize(equalized.size());
    if (mode.scheme == TransmissionScheme::alamouti_stbc) {
        const auto pairs = layout.payload_time_indices.size()/2;
        constexpr float scale = 0.70710678118655f;
        for (std::size_t i = 0; i < pairs; ++i) {
            const auto k = layout.data_fft_indices[layout.payload_data_positions[i]];
            Complex a{}, b{};
            float gain = 0;
            for (unsigned rx = 0; rx < ports; ++rx) {
                const auto row = rx*antennas_max;
                const auto h0 = (channels[k].values[row]+channels[nfft+k].values[row])*0.5f;
                const auto h1 = (channels[k].values[row+1]+channels[nfft+k].values[row+1])*0.5f;
                const auto y0 = grid[k*ports+rx], y1 = grid[(nfft+k)*ports+rx];
                a += std::conj(h0)*y0+h1*std::conj(y1);
                b += std::conj(h1)*y0-h0*std::conj(y1);
                gain += std::norm(h0)+std::norm(h1);
            }
            gain = std::max(gain, 1e-12f);
            equalized[i] = a/(scale*gain);
            equalized[i+pairs] = b/(scale*gain);
            variance[i] = variance[i+pairs] = 2*noise/gain;
        }
        return;
    }
    for (std::size_t i = 0; i < layout.payload_time_indices.size(); ++i) {
        const auto k = layout.data_fft_indices[layout.payload_data_positions[i]];
        const auto index = layout.payload_time_indices[i]*nfft+k;
        std::array<Complex, antennas_max> values{};
        std::copy_n(grid.begin()+index*ports, ports, values.begin());
        const auto detected = detect_nxn(values, channels[index], noise*0.5f, LinearDetector::mmse);
        for (unsigned layer = 0; layer < mode.rank; ++layer) {
            equalized[i*mode.rank+layer] = detected.symbols[layer];
            variance[i*mode.rank+layer] = std::max(detected.predicted_mse[layer], 1e-10f);
        }
    }
}

void estimate_evm(const std::vector<Complex>& equalized, const std::vector<Complex>& ideal,
                  LinkMode mode, Metrics& info) {
    const auto bits = modulation_bits(mode.modulation);
    std::array<double, antennas_max> errors{}, references{};
    for (std::size_t i = 0; i < info.coded_symbols; ++i) {
        const auto layer = i % mode.rank;
        const auto hard = SquareQAM::demodulate(equalized[i], bits);
        const auto reference = ideal.empty() ? SquareQAM::modulate(hard, bits) : ideal[i];
        errors[layer] += std::norm(equalized[i]-reference);
        references[layer] += std::norm(reference);
        if (!ideal.empty()) {
            auto wrong = hard ^ SquareQAM::demodulate(reference, bits);
            for (; wrong; wrong &= wrong-1)
                ++info.bit_errors[layer];
            info.compared_bits[layer] += bits;
        }
    }
    for (unsigned layer = 0; layer < mode.rank; ++layer)
        info.evm[layer] = static_cast<float>(100*std::sqrt(errors[layer]/std::max(references[layer], 1e-20)));
}
}

STUDIO_API unsigned studio_isac_abi() noexcept { return 7; }

STUDIO_API void* studio_isac_open(const char* matrices, char* error) noexcept {
    Codec* result = nullptr;
    run(error, [&] { result = new Codec(matrices); });
    return result;
}
STUDIO_API void studio_isac_close(void* codec) noexcept { delete static_cast<Codec*>(codec); }

STUDIO_API int studio_isac_profile(unsigned ports, unsigned stbc, unsigned bits, unsigned edge,
                                   uint32_t* capacity, uint32_t* symbols, char* error) noexcept {
    return run(error, [&] {
        const auto layout = frame_layout(mode_for(ports, stbc != 0, bits), edge);
        *capacity = static_cast<uint32_t>(layout.user_payload_bytes);
        *symbols = static_cast<uint32_t>(layout.payload_layer_symbols);
    });
}

STUDIO_API void studio_isac_preamble(void* context, unsigned edge, float* output) noexcept {
    const auto& reference = preamble_for(*static_cast<Codec*>(context), edge);
    for (std::size_t i = 0; i < reference.size(); ++i)
        write_pair(output, i, reference[i]);
}

STUDIO_API int studio_isac_encode(void* context, unsigned ports, unsigned stbc, unsigned bits, unsigned dmrs,
                                  unsigned edge, unsigned sequence, const uint8_t* bytes, std::size_t count,
                                  float* iq, float* constellation, char* error) noexcept {
    return run(error, [&] {
        auto& codec = *static_cast<Codec*>(context);
        const auto mode = mode_for(ports, stbc != 0, bits);
        const std::vector<uint8_t> payload(bytes, bytes+count);
        const auto frame = encode_frame(payload, mode, static_cast<uint16_t>(sequence), dmrs != 0, edge, codec);
        const auto& preamble = preamble_for(codec, edge);
        const auto frame_size = formal_frame_symbols(pilot_mode(dmrs != 0))*symbol_size;
        for (std::size_t i = 0; i < frame.payload_symbols.size(); ++i)
            write_pair(constellation, i, frame.payload_symbols[i]);
        std::vector<Complex> references, tones(nfft);
        if (dmrs) {
            if (ports == 8)
                studio_rank8::pilots(true, references, edge);
            else
                build_nr_dmrs_reference_grid(nfft, frame.layout.active_fft_indices, ports, pilot_seed, references);
        }
        for (unsigned tx = 0; tx < ports; ++tx) {
            for (std::size_t i = 0; i < symbol_size; ++i)
                write_pair(iq, tx*frame_size+i, tx == 0 ? preamble[i] : Complex{});
            for (std::size_t time = 1; time < frame_size/symbol_size; ++time) {
                const bool pilot = dmrs && time < 3;
                const auto& grid = pilot ? references : frame.tx_grid;
                const auto grid_symbol = time-(pilot || !dmrs ? 1 : 3);
                for (std::size_t k = 0; k < nfft; ++k)
                    tones[k] = grid[(grid_symbol*nfft+k)*ports+tx];
                const auto samples = ofdm_modulate(tones, cp);
                for (std::size_t i = 0; i < symbol_size; ++i)
                    write_pair(iq, tx*frame_size+time*symbol_size+i, samples[i]);
            }
        }
    });
}

STUDIO_API int studio_isac_decode(void* context, unsigned ports, unsigned stbc, unsigned dmrs, unsigned edge,
                                  const float* iq, uint8_t* bytes, float* symbols, float* csi,
                                  Metrics* info, char* error) noexcept {
    return run(error, [&] {
        auto& codec = *static_cast<Codec*>(context);
        *info = {};
        const auto frame_symbols = formal_frame_symbols(pilot_mode(dmrs != 0));
        const auto count = frame_symbols*symbol_size;
        std::vector<std::vector<Complex>> receive(ports, std::vector<Complex>(count));
        for (unsigned rx = 0; rx < ports; ++rx)
            for (std::size_t i = 0; i < count; ++i)
                receive[rx][i] = read_pair(iq, rx*count+i);
        const auto cfo = estimate_cp_cfo_normalized(receive, 0, nfft, cp, frame_symbols, 16);
        info->cfo_hz = cfo*spacing;
        apply_cfo_normalized_inplace(receive, -cfo, nfft);
        auto grid = fft_grid(receive, dmrs ? 3 : 1);
        auto mode = mode_for(ports, stbc != 0, 2);
        auto layout = frame_layout(mode, edge);
        std::vector<ChannelNxN> channels;
        float noise, marker;
        estimate_channels(grid, receive, layout, mode, dmrs != 0, edge, *info, channels, noise);
        const auto header = frame_header(grid, channels, layout, ports, dmrs != 0, noise, marker);
        if (ports != 8 && (transmit_rank_from_flags(header.flags) != mode.rank ||
            transmission_scheme_from_flags(header.flags) != mode.scheme))
            throw std::runtime_error("OpenISAC header topology doesn't match the RX selection");
        info->bits = bits_per_symbol_from_flags(header.flags);
        mode = mode_for(ports, stbc != 0, info->bits);
        layout = frame_layout(mode, edge);
        std::vector<Complex> equalized, ideal;
        std::vector<float> variance;
        payload_symbols(grid, channels, layout, mode, noise, equalized, variance);
        DecodedDynamicFrame decoded;
        if (edge != legacy_edge || ports == 8) {
            decoded = studio_rank8::decode_payload(header, mode, layout, equalized, variance, codec.ldpc);
        } else {
            PreparedDynamicFrame prepared;
            prepare_dynamic_frame_payload_llrs(header, marker, mode, equalized, variance, prepared);
            decoded = decode_prepared_dynamic_frame(prepared, codec.ldpc, 10);
        }
        info->sequence = header.sequence;
        info->crc = decoded.crc_ok;
        info->coded_symbols = static_cast<uint32_t>(std::min(
            header.payload_blocks*ldpc_encoded_bits/info->bits, layout.coded_qam_symbols));
        info->symbols = symbols != nullptr ? static_cast<uint32_t>(equalized.size()) : 0;
        if (decoded.crc_ok) {
            info->payload_bytes = static_cast<uint32_t>(decoded.user_payload.size());
            std::copy(decoded.user_payload.begin(), decoded.user_payload.end(), bytes);
            if (symbols != nullptr)
                ideal = encode_frame(decoded.user_payload, mode, header.sequence, dmrs != 0, edge, codec).payload_symbols;
        }
        if (symbols != nullptr) {
            estimate_evm(equalized, ideal, mode, *info);
            for (std::size_t i = 0; i < equalized.size(); ++i)
                write_pair(symbols, i, equalized[i]);
        }
        if (csi != nullptr) {
            std::fill_n(csi, 2*ports*ports*nfft, 0.0f);
            for (unsigned rx = 0; rx < ports; ++rx) {
                for (unsigned tx = 0; tx < ports; ++tx) {
                    const auto link = rx*ports+tx;
                    const auto component = rx*antennas_max+tx;
                    for (auto k : layout.active_fft_indices)
                        write_pair(csi, link*nfft+k, (channels[k].values[component] +
                                                      channels[nfft+k].values[component])*0.5f);
                }
            }
        }
    });
}

// A batch is a contiguous coherent processing interval. The caller checks
// the sample-time lattice before passing it to the processor.
STUDIO_API int studio_isac_sense(unsigned ports, unsigned count, float frame_period,
                                 float rf_hz, unsigned reject_static, unsigned sample_rate, const float* cube,
                                 float* power, Detection* detections, uint32_t* detection_count,
                                 float* bins, char* error) noexcept {
    return run(error, [&] {
        DynamicSensingConfig config;
        config.transmit_ports = config.receive_ports = ports;
        config.coherent_frames = config.doppler_fft_size = count;
        config.frame_period_seconds = frame_period;
        config.center_frequency_hz = rf_hz;
        config.subcarrier_spacing_hz = static_cast<float>(sample_rate)/nfft;
        config.enable_static_clutter_suppression = reject_static != 0;
        DynamicSensingProcessor processor(config);
        const auto stride = ports*ports*nfft;
        std::vector<Complex> response(stride);
        std::vector<uint8_t> active(nfft);
        const auto edge = static_cast<int>(limited_edge);
        for (int k = -edge; k <= edge; ++k)
            if (k != 0)
                active[(k+static_cast<int>(nfft))%nfft] = 1;
        for (unsigned frame = 0; frame < count; ++frame) {
            for (std::size_t i = 0; i < stride; ++i)
                response[i] = read_pair(cube, frame*stride+i);
            processor.push_channel_frame(frame, frame, response, active);
        }
        const auto& result = processor.last_result();
        if (!result.ready)
            throw std::runtime_error("coherent processing interval did not complete");
        for (unsigned d = 0; d < count; ++d)
            for (std::size_t r = 0; r < nfft/2; ++r)
                power[d*(nfft/2)+r] = std::norm(result.range_doppler_map[d*nfft+r]);
        *detection_count = static_cast<uint32_t>(result.detections.size());
        for (std::size_t i = 0; i < result.detections.size(); ++i) {
            const auto& hit = result.detections[i];
            detections[i] = {hit.peak.range_m, hit.peak.doppler_hz, hit.peak.velocity_mps,
                             hit.power_over_threshold_db};
        }
        bins[0] = result.range_bin_spacing_m;
        bins[1] = result.doppler_bin_spacing_hz;
        bins[2] = result.velocity_bin_spacing_mps;
    });
}
