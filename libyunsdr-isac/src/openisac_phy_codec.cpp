#include "libyunsdr_isac/openisac_phy_codec.hpp"

#include "openisac/binary_io.hpp"
#include "openisac/dynamic_link.hpp"
#include "openisac/frame.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/ldpc_frame_decoder.hpp"
#include "openisac/link_adaptation.hpp"
#include "openisac/qam.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef LIBYUNSDR_ISAC_MATRIX_DIR
#define LIBYUNSDR_ISAC_MATRIX_DIR ""
#endif

namespace libyunsdr_isac {
namespace {

openisac::Modulation to_openisac(PhyModulation modulation) {
    switch (modulation) {
        case PhyModulation::qpsk: return openisac::Modulation::qpsk;
        case PhyModulation::qam16: return openisac::Modulation::qam16;
        case PhyModulation::qam64: return openisac::Modulation::qam64;
        case PhyModulation::qam256: return openisac::Modulation::qam256;
    }
    throw std::invalid_argument("unsupported PHY modulation");
}

openisac::PilotMode to_openisac(PilotPattern pilot) {
    return pilot == PilotPattern::nr_dmrs
        ? openisac::PilotMode::nr_dmrs
        : openisac::PilotMode::fdm;
}

openisac::LinkMode to_link_mode(
    PhyMode mode,
    PhyModulation modulation) {
    const auto openisac_modulation = to_openisac(modulation);
    switch (mode) {
        case PhyMode::siso:
            return {1u, openisac_modulation,
                    openisac::TransmissionScheme::spatial_multiplexing, 1u};
        case PhyMode::spatial_2x2:
            return {2u, openisac_modulation,
                    openisac::TransmissionScheme::spatial_multiplexing, 2u};
        case PhyMode::alamouti_stbc_2x2:
            return {1u, openisac_modulation,
                    openisac::TransmissionScheme::alamouti_stbc, 2u};
    }
    throw std::invalid_argument("unsupported OpenISAC PHY mode");
}

class OpenIsacPhyCodec final : public IPhyFrameCodec {
public:
    explicit OpenIsacPhyCodec(OpenIsacPhyCodecConfig config)
        : config_(std::move(config)),
          profile_(make_mode_profile(config_.mode)),
          mode_(to_link_mode(config_.mode, config_.modulation)),
          pilot_(to_openisac(config_.pilot_pattern)),
          matrix_directory_(config_.matrix_directory.empty()
              ? std::string(LIBYUNSDR_ISAC_MATRIX_DIR)
              : config_.matrix_directory),
          ldpc_(openisac::join_path(matrix_directory_, "LDPC_504_1008.alist"),
                openisac::join_path(matrix_directory_, "LDPC_504_1008G.alist")),
          decoder_(ldpc_, std::max<std::size_t>(1u, config_.ldpc_workers)) {
        if (matrix_directory_.empty()) {
            throw std::invalid_argument("OpenISAC LDPC matrix directory is empty");
        }
        if (!std::isfinite(config_.transmit_peak) ||
            config_.transmit_peak <= 0.0f || config_.transmit_peak > 1.0f) {
            throw std::invalid_argument("transmit peak must be in (0, 1]");
        }
        if (config_.maximum_channel_delay_samples >= 128u) {
            throw std::invalid_argument("maximum channel delay must fit the CP");
        }

        openisac::FormalFrameProfile layout_profile;
        layout_profile.transmit_rank = mode_.rank;
        layout_profile.bits_per_symbol =
            openisac::modulation_bits(mode_.modulation);
        layout_profile.scheme = mode_.scheme;
        maximum_payload_bytes_ =
            openisac::build_formal_frame_layout(layout_profile).user_payload_bytes;

        receiver_config_.pilot_mode = pilot_;
        receiver_config_.noise_variance_mode =
            openisac::NoiseVarianceMode::pilot_residual;
        receiver_config_.maximum_timing_offset_samples =
            config_.maximum_timing_offset_samples;
        receiver_config_.maximum_channel_delay_samples =
            config_.maximum_channel_delay_samples;
        receiver_config_.csi_smoothing_alpha = config_.csi_smoothing_alpha;
    }

    ModeProfile mode_profile() const noexcept override { return profile_; }
    PilotPattern pilot_pattern() const noexcept override {
        return config_.pilot_pattern;
    }
    std::size_t frame_samples_per_port() const noexcept override {
        return openisac::formal_frame_symbols(pilot_) * (1024u + 128u);
    }
    std::size_t maximum_payload_bytes() const noexcept override {
        return maximum_payload_bytes_;
    }

    std::size_t encode(
        ByteView payload,
        std::uint16_t sequence,
        std::uint32_t pilot_seed,
        const MutableMultiChannelBuffer& output) override {
        if (payload.data == nullptr || payload.size == 0u ||
            payload.size > maximum_payload_bytes_) {
            throw std::invalid_argument("payload does not fit the selected PHY mode");
        }
        validate_output(output, profile_.tx_ports, frame_samples_per_port());
        std::vector<std::uint8_t> bytes(payload.data, payload.data + payload.size);
        openisac::DynamicLinkTransmitFrame frame;
        openisac::generate_dynamic_tx_iq_frame(
            bytes, mode_, sequence, ldpc_, pilot_, pilot_seed, frame,
            generation_workspace_);
        if (frame.samples.size() != profile_.tx_ports) {
            throw std::runtime_error("OpenISAC generated the wrong TX port count");
        }

        float maximum = 0.0f;
        for (const auto& branch : frame.samples) {
            for (const auto sample : branch) {
                maximum = std::max(maximum, std::abs(sample));
            }
        }
        if (!(maximum > 0.0f)) {
            throw std::runtime_error("OpenISAC generated an empty waveform");
        }
        const float scale = config_.transmit_peak / maximum;
        for (std::size_t port = 0u; port < profile_.tx_ports; ++port) {
            std::transform(
                frame.samples[port].begin(), frame.samples[port].end(),
                output.channels[port],
                [scale](std::complex<float> sample) { return sample * scale; });
        }
        return frame.samples.front().size();
    }

    PhyDecodeResult decode(
        const ConstMultiChannelBuffer& capture,
        std::uint64_t capture_timestamp,
        std::uint32_t pilot_seed,
        MutableByteView payload_output) override {
        validate_input(capture, profile_.rx_ports);
        if (payload_output.data == nullptr || payload_output.capacity == 0u) {
            throw std::invalid_argument("decode payload output is empty");
        }

        openisac::DynamicLinkCaptureFrame captured;
        captured.timestamp = capture_timestamp;
        captured.capture_sequence = capture_sequence_++;
        captured.pilot_seed = pilot_seed;
        captured.samples.resize(capture.channel_count);
        for (std::size_t port = 0u; port < capture.channel_count; ++port) {
            captured.samples[port].assign(
                capture.channels[port],
                capture.channels[port] + capture.samples_per_channel);
        }

        openisac::PreparedDynamicLinkFrame prepared;
        openisac::prepare_captured_iq_frame(
            captured, receiver_config_, prepared, &receiver_state_,
            receiver_workspace_);
        const auto decoded = openisac::finish_dynamic_tdl_frame(
            prepared, ldpc_, receiver_workspace_, &decoder_);

        PhyDecodeResult result;
        result.timing_ok = decoded.timing_ok;
        result.header_ok = decoded.header_ok;
        result.crc_ok = decoded.crc_ok;
        result.sequence = decoded.sequence;
        result.timing_metric = decoded.timing_metric;
        result.cfo_hz = decoded.cfo_error_hz;
        result.evm_percent = decoded.evm_percent;
        if (decoded.crc_ok) {
            if (decoded.user_payload.size() > payload_output.capacity) {
                throw std::runtime_error("decoded payload buffer is too small");
            }
            std::copy(decoded.user_payload.begin(), decoded.user_payload.end(),
                      payload_output.data);
            result.payload_bytes = decoded.user_payload.size();
        }
        if (decoded.header_ok) {
            update_telemetry(capture, capture_timestamp, decoded, result);
        }
        return result;
    }

    bool copy_telemetry(PhyTelemetrySnapshot& output) const override {
        if (!telemetry_.valid) return false;
        output = telemetry_;
        return true;
    }

private:
    void update_telemetry(
        const ConstMultiChannelBuffer& capture,
        std::uint64_t capture_timestamp,
        const openisac::DynamicLinkSimulationResult& decoded,
        const PhyDecodeResult& result) {
        constexpr std::size_t fft_size = 1024u;
        constexpr std::size_t waveform_points = 4096u;
        telemetry_.valid = true;
        telemetry_.decode = result;
        telemetry_.capture_timestamp = capture_timestamp;
        telemetry_.timing_offset_samples = receiver_workspace_.timing_estimate.offset;
        telemetry_.residual_sfo_ppm = decoded.residual_sfo_ppm;
        telemetry_.noise_variance = decoded.noise_variance_used;

        const std::size_t samples = std::min(
            capture.samples_per_channel, waveform_points);
        telemetry_.receive_waveform_rx0.assign(
            capture.channels[0], capture.channels[0] + samples);

        const unsigned bits = openisac::modulation_bits(mode_.modulation);
        std::size_t symbols = receiver_workspace_.equalized.size();
        if (decoded.crc_ok) {
            const std::size_t information_bytes = decoded.user_payload.size() + 2u;
            const std::size_t payload_blocks = (information_bytes + 62u) / 63u;
            symbols = std::min(
                symbols, payload_blocks * 1008u / bits);
        }
        telemetry_.constellation_equalized.assign(
            receiver_workspace_.equalized.begin(),
            receiver_workspace_.equalized.begin() + symbols);
        telemetry_.constellation_ideal.resize(symbols);
        for (std::size_t index = 0u; index < symbols; ++index) {
            const auto equalized = telemetry_.constellation_equalized[index];
            telemetry_.constellation_ideal[index] = openisac::SquareQAM::modulate(
                openisac::SquareQAM::demodulate(equalized, bits), bits);
        }

        const auto& channel_view = receiver_state_.csi_valid
            ? receiver_state_.filtered_channels
            : receiver_workspace_.channels;
        telemetry_.channel_frequency_response.clear();
        if (channel_view.size() >= 2u * fft_size) {
            telemetry_.channel_frequency_response.resize(fft_size);
            for (std::size_t fft = 0u; fft < fft_size; ++fft) {
                const auto& first = channel_view[fft];
                const auto& second = channel_view[fft_size + fft];
                telemetry_.channel_frequency_response[fft] = {
                    0.5f * (first.h00 + second.h00),
                    0.5f * (first.h01 + second.h01),
                    0.5f * (first.h10 + second.h10),
                    0.5f * (first.h11 + second.h11)};
            }
        }
    }

    static void validate_output(
        const MutableMultiChannelBuffer& output,
        std::size_t ports,
        std::size_t samples) {
        if (output.channel_count != ports ||
            output.samples_per_channel < samples) {
            throw std::invalid_argument("PHY TX output buffer has wrong dimensions");
        }
        for (std::size_t port = 0u; port < ports; ++port) {
            if (output.channels[port] == nullptr) {
                throw std::invalid_argument("PHY TX output contains null channel");
            }
        }
    }

    static void validate_input(
        const ConstMultiChannelBuffer& input,
        std::size_t ports) {
        if (input.channel_count != ports || input.samples_per_channel == 0u) {
            throw std::invalid_argument("PHY RX input buffer has wrong dimensions");
        }
        for (std::size_t port = 0u; port < ports; ++port) {
            if (input.channels[port] == nullptr) {
                throw std::invalid_argument("PHY RX input contains null channel");
            }
        }
    }

    OpenIsacPhyCodecConfig config_;
    ModeProfile profile_;
    openisac::LinkMode mode_;
    openisac::PilotMode pilot_;
    std::string matrix_directory_;
    openisac::Ldpc5041008 ldpc_;
    openisac::LdpcFrameDecoder decoder_;
    std::size_t maximum_payload_bytes_ = 0u;
    std::uint64_t capture_sequence_ = 0u;
    openisac::DynamicLinkReceiverConfig receiver_config_;
    openisac::DynamicLinkReceiverState receiver_state_;
    openisac::DynamicLinkWorkspace generation_workspace_;
    openisac::DynamicLinkWorkspace receiver_workspace_;
    PhyTelemetrySnapshot telemetry_{};
};

}  // namespace

std::unique_ptr<IPhyFrameCodec> make_openisac_phy_codec(
    const OpenIsacPhyCodecConfig& config) {
    return std::unique_ptr<IPhyFrameCodec>(new OpenIsacPhyCodec(config));
}

}  // namespace libyunsdr_isac
