#include "openisac/binary_io.hpp"
#include "openisac/dynamic_link.hpp"
#include "openisac/link_adaptation.hpp"
#include "openisac/ldpc.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

const char* reason_name(openisac::ControllerReason reason) {
    switch (reason) {
        case openisac::ControllerReason::hold: return "hold";
        case openisac::ControllerReason::quality_downshift: return "quality_downshift";
        case openisac::ControllerReason::crc_fast_downshift: return "crc_fast_downshift";
        case openisac::ControllerReason::outage_fast_downshift: return "outage_fast_downshift";
        case openisac::ControllerReason::upshift_hysteresis: return "upshift_hysteresis";
        case openisac::ControllerReason::confirmed_step_upshift: return "confirmed_step_upshift";
    }
    return "unknown";
}

std::string mode_name(const openisac::LinkMode& mode) {
    return "r" + std::to_string(mode.rank) + "_" +
           openisac::modulation_name(mode.modulation);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("measurement/cpp_adaptive_tdl/frames.csv");
        const std::size_t frames_per_snr = argc > 2
            ? static_cast<std::size_t>(std::stoul(argv[2]))
            : 20u;
        if (frames_per_snr < 4u) {
            throw std::invalid_argument("at least four frames per SNR are required");
        }
        std::filesystem::create_directories(output.parent_path());
        std::ofstream csv(output);
        if (!csv) {
            throw std::runtime_error("cannot write " + output.string());
        }
        csv << "snr_db,frame,seed,adaptive_mode,adaptive_rank,adaptive_qm,"
               "adaptive_payload_bytes,adaptive_timing_ok,adaptive_header_ok,"
               "adaptive_crc_ok,adaptive_goodput_bps,adaptive_evm_percent,"
               "adaptive_channel_nmse_db,adaptive_marker_metric,"
               "adaptive_csi_smoothed,adaptive_csi_age_frames,"
               "desired_mode,next_mode,controller_reason,"
               "rank1_sinr_db,rank2_bottleneck_sinr_db,eigenvalue_ratio,"
               "fixed_mode,fixed_payload_bytes,fixed_timing_ok,fixed_header_ok,"
               "fixed_crc_ok,fixed_goodput_bps,fixed_evm_percent,"
               "fixed_channel_nmse_db,fixed_marker_metric,"
               "smoothed_payload_bytes,smoothed_timing_ok,smoothed_header_ok,"
               "smoothed_crc_ok,smoothed_goodput_bps,smoothed_evm_percent,"
               "smoothed_channel_nmse_db,smoothed_marker_metric,"
               "smoothed_csi_age_frames\n";
        csv << std::setprecision(10);

        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        constexpr std::array<float, 5> snr_values{{28.0f, 32.0f, 36.0f, 40.0f, 44.0f}};
        const openisac::LinkMode fixed_mode{2u, openisac::Modulation::qam64};

        for (std::size_t snr_index = 0u; snr_index < snr_values.size(); ++snr_index) {
            openisac::AdaptiveLinkController controller(
                {1u, openisac::Modulation::qpsk}, 3u);
            openisac::DynamicLinkReceiverState adaptive_state;
            openisac::DynamicLinkReceiverState smoothed_fixed_state;
            openisac::DynamicLinkWorkspace adaptive_workspace;
            openisac::DynamicLinkWorkspace fixed_workspace;
            openisac::DynamicLinkWorkspace smoothed_fixed_workspace;
            std::size_t adaptive_failures = 0u;
            std::size_t fixed_failures = 0u;
            std::size_t smoothed_failures = 0u;
            double adaptive_goodput = 0.0;
            double fixed_goodput = 0.0;
            double smoothed_goodput = 0.0;
            for (std::size_t frame = 0u; frame < frames_per_snr; ++frame) {
                openisac::DynamicLinkSimulationConfig config;
                config.snr_db = snr_values[snr_index];
                config.random_seed = static_cast<unsigned>(
                    0x6100u + snr_index * 1000u + frame * 17u);
                config.csi_smoothing_alpha = 0.25f;
                const auto adaptive_mode = controller.current();
                const auto adaptive = openisac::simulate_dynamic_tdl_frame(
                    adaptive_mode, static_cast<std::uint16_t>(frame), codec,
                    config, &adaptive_state, &adaptive_workspace);
                const auto fixed = openisac::simulate_dynamic_tdl_frame(
                    fixed_mode, static_cast<std::uint16_t>(frame), codec,
                    config, nullptr, &fixed_workspace);
                const auto smoothed = openisac::simulate_dynamic_tdl_frame(
                    fixed_mode, static_cast<std::uint16_t>(frame), codec,
                    config, &smoothed_fixed_state, &smoothed_fixed_workspace);
                const bool adaptive_failure =
                    !adaptive.timing_ok || !adaptive.header_ok || !adaptive.crc_ok;
                const bool fixed_failure =
                    !fixed.timing_ok || !fixed.header_ok || !fixed.crc_ok;
                const bool smoothed_failure =
                    !smoothed.timing_ok || !smoothed.header_ok || !smoothed.crc_ok;
                const auto update = controller.observe(
                    adaptive.recommendation.desired,
                    adaptive_failure,
                    adaptive.recommendation.outage || !adaptive.timing_ok);
                adaptive_failures += adaptive_failure;
                fixed_failures += fixed_failure;
                smoothed_failures += smoothed_failure;
                adaptive_goodput += adaptive.goodput_bps;
                fixed_goodput += fixed.goodput_bps;
                smoothed_goodput += smoothed.goodput_bps;
                csv << snr_values[snr_index] << ',' << frame << ','
                    << config.random_seed << ',' << mode_name(adaptive_mode) << ','
                    << adaptive_mode.rank << ','
                    << openisac::modulation_bits(adaptive_mode.modulation) << ','
                    << adaptive.user_payload_bytes << ',' << adaptive.timing_ok << ','
                    << adaptive.header_ok << ',' << adaptive.crc_ok << ','
                    << adaptive.goodput_bps << ',' << adaptive.evm_percent << ','
                    << adaptive.channel_nmse_db << ',' << adaptive.marker_metric << ','
                    << adaptive.csi_smoothed << ',' << adaptive.csi_age_frames << ','
                    << mode_name(adaptive.recommendation.desired) << ','
                    << mode_name(update.selected) << ',' << reason_name(update.reason) << ','
                    << adaptive.recommendation.rank1_sinr_db << ','
                    << adaptive.recommendation.rank2_bottleneck_sinr_db << ','
                    << adaptive.recommendation.minimum_eigenvalue_ratio << ','
                    << mode_name(fixed_mode) << ',' << fixed.user_payload_bytes << ','
                    << fixed.timing_ok << ',' << fixed.header_ok << ',' << fixed.crc_ok << ','
                    << fixed.goodput_bps << ',' << fixed.evm_percent << ','
                    << fixed.channel_nmse_db << ',' << fixed.marker_metric << ','
                    << smoothed.user_payload_bytes << ',' << smoothed.timing_ok << ','
                    << smoothed.header_ok << ',' << smoothed.crc_ok << ','
                    << smoothed.goodput_bps << ',' << smoothed.evm_percent << ','
                    << smoothed.channel_nmse_db << ',' << smoothed.marker_metric << ','
                    << smoothed.csi_age_frames << '\n';
            }
            std::cout << "SNR " << snr_values[snr_index]
                      << " dB: adaptive FER="
                      << static_cast<double>(adaptive_failures) /
                             static_cast<double>(frames_per_snr)
                      << ", fixed FER="
                      << static_cast<double>(fixed_failures) /
                             static_cast<double>(frames_per_snr)
                      << ", smoothed-fixed FER="
                      << static_cast<double>(smoothed_failures) /
                             static_cast<double>(frames_per_snr)
                      << ", adaptive/fixed/smoothed goodput="
                      << adaptive_goodput / static_cast<double>(frames_per_snr) / 1.0e6
                      << '/'
                      << fixed_goodput / static_cast<double>(frames_per_snr) / 1.0e6
                      << '/'
                      << smoothed_goodput / static_cast<double>(frames_per_snr) / 1.0e6
                      << " Mbit/s\n";
        }
        std::cout << "Wrote " << output.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
