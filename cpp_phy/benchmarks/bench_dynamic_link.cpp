#include "openisac/binary_io.hpp"
#include "openisac/dynamic_link.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/ldpc_frame_decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        throw std::invalid_argument("percentile requires samples");
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1u) + 0.5);
    return values[std::min(index, values.size() - 1u)];
}

double mean(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

struct Variant {
    std::string name;
    std::size_t configured_workers = 0u;
    std::unique_ptr<openisac::DynamicLinkWorkspace> workspace;
    std::unique_ptr<openisac::LdpcFrameDecoder> decoder;
    std::unique_ptr<openisac::DynamicLinkReceiverState> receiver_state;
    std::vector<double> receiver;
    std::vector<double> simulation;
    std::vector<double> pipeline_interval;
};

Variant legacy_variant(const std::string& name, bool reuse_workspace) {
    Variant result;
    result.name = name;
    if (reuse_workspace) {
        result.workspace.reset(new openisac::DynamicLinkWorkspace);
    }
    return result;
}

Variant pool_variant(
    const openisac::Ldpc5041008& codec,
    std::size_t workers,
    bool continuous_tracking = false) {
    Variant result;
    result.name = "pool_" + std::to_string(workers) +
        (continuous_tracking ? "_track" : "");
    result.configured_workers = workers;
    result.workspace.reset(new openisac::DynamicLinkWorkspace);
    result.decoder.reset(new openisac::LdpcFrameDecoder(codec, workers));
    if (continuous_tracking) {
        result.receiver_state.reset(new openisac::DynamicLinkReceiverState);
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("measurement/cpp_realtime_benchmark/frames.csv");
        const std::size_t frames = argc > 2
            ? static_cast<std::size_t>(std::stoul(argv[2]))
            : 200u;
        if (frames < 20u) {
            throw std::invalid_argument("at least 20 timed frames are required");
        }
        std::filesystem::create_directories(output.parent_path());
        std::ofstream csv(output);
        if (!csv) {
            throw std::runtime_error("cannot write " + output.string());
        }
        csv << "frame,seed,variant,configured_workers,crc_ok,receiver_us,"
               "receiver_wall_us,simulation_us,transmit_us,channel_us,sync_us,"
               "fft_csi_us,detection_us,control_header_us,soft_demapping_us,"
               "ldpc_crc_us,control_fec_us,frontend_us,pipeline_interval_us,"
               "diagnostics_us,"
               "workspace_growths,ldpc_growths,sync_mode,tracking_fallback,"
               "timing_candidates\n";
        csv << std::setprecision(10);

        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        openisac::DynamicLinkSimulationConfig config;
        config.snr_db = 40.0f;
        config.csi_smoothing_alpha = 1.0f;
        const openisac::LinkMode mode{2u, openisac::Modulation::qam64};

        std::vector<Variant> variants;
        variants.reserve(7u);
        variants.push_back(legacy_variant("local_legacy", false));
        variants.push_back(legacy_variant("reused_legacy", true));
        for (const std::size_t workers : {1u, 2u, 4u, 8u}) {
            variants.push_back(pool_variant(codec, workers));
        }
        variants.push_back(pool_variant(codec, 8u, true));
        for (auto& variant : variants) {
            variant.receiver.reserve(frames);
            variant.simulation.reserve(frames);
            variant.pipeline_interval.reserve(frames);
        }

        // Warm the largest frame shape twice. The second call proves that both
        // top-level and decoder-internal retained buffers are steady-state.
        for (std::size_t index = 1u; index < variants.size(); ++index) {
            auto& variant = variants[index];
            config.random_seed = static_cast<unsigned>(0x9000u + index);
            const auto first = openisac::simulate_dynamic_tdl_frame(
                {2u, openisac::Modulation::qam256}, 0u, codec, config,
                variant.receiver_state.get(), variant.workspace.get(),
                variant.decoder.get());
            const auto second = openisac::simulate_dynamic_tdl_frame(
                {2u, openisac::Modulation::qam256}, 1u, codec, config,
                variant.receiver_state.get(), variant.workspace.get(),
                variant.decoder.get());
            if (!first.crc_ok || !second.crc_ok ||
                first.workspace_growths_this_frame == 0u ||
                second.workspace_growths_this_frame != 0u ||
                (variant.decoder != nullptr &&
                 (first.ldpc_capacity_growths_this_frame == 0u ||
                  second.ldpc_capacity_growths_this_frame != 0u))) {
                throw std::runtime_error(
                    "steady-state warm-up failed for " + variant.name);
            }
            if (variant.receiver_state != nullptr &&
                second.synchronization_mode_used !=
                    openisac::SynchronizationMode::track) {
                throw std::runtime_error(
                    "tracking warm-up failed for " + variant.name);
            }
        }

        std::vector<openisac::DynamicLinkSimulationResult> results(variants.size());
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            config.random_seed = static_cast<unsigned>(0x9100u + frame * 29u);
            // Rotate execution order across all variants to reduce cache/turbo bias.
            const std::size_t start = frame % variants.size();
            for (std::size_t pass = 0u; pass < variants.size(); ++pass) {
                const std::size_t index = (start + pass) % variants.size();
                auto& variant = variants[index];
                results[index] = openisac::simulate_dynamic_tdl_frame(
                    mode, static_cast<std::uint16_t>(frame), codec, config,
                    variant.receiver_state.get(), variant.workspace.get(),
                    variant.decoder.get());
            }
            const auto& reference = results.front();
            for (std::size_t index = 0u; index < variants.size(); ++index) {
                auto& variant = variants[index];
                const auto& result = results[index];
                if (!result.crc_ok || result.decoded_mode != reference.decoded_mode ||
                    result.syndrome_failures != reference.syndrome_failures ||
                    (variant.workspace != nullptr &&
                     result.workspace_growths_this_frame != 0u) ||
                    (variant.decoder != nullptr &&
                     result.ldpc_capacity_growths_this_frame != 0u)) {
                    throw std::runtime_error(
                        "timed variant changed results or grew warmed buffers: " +
                        variant.name);
                }
                variant.receiver.push_back(result.timing.receiver_total_us);
                variant.simulation.push_back(result.timing.simulation_total_us);
                variant.pipeline_interval.push_back(std::max(
                    result.timing.receiver_total_us - result.timing.ldpc_crc_us,
                    result.timing.ldpc_crc_us));
                csv << frame << ',' << config.random_seed << ',' << variant.name << ','
                    << variant.configured_workers << ',' << result.crc_ok << ','
                    << result.timing.receiver_total_us << ','
                    << result.timing.receiver_wall_us << ','
                    << result.timing.simulation_total_us << ','
                    << result.timing.transmit_prepare_us << ','
                    << result.timing.channel_impairments_us << ','
                    << result.timing.synchronization_us << ','
                    << result.timing.fft_csi_us << ','
                    << result.timing.detection_adaptation_us << ','
                    << result.timing.control_header_us << ','
                    << result.timing.soft_demapping_us << ','
                    << result.timing.ldpc_crc_us << ','
                    << result.timing.control_fec_us << ','
                    << result.timing.receiver_total_us - result.timing.ldpc_crc_us << ','
                    << std::max(
                           result.timing.receiver_total_us - result.timing.ldpc_crc_us,
                           result.timing.ldpc_crc_us) << ','
                    << result.timing.diagnostics_us << ','
                    << result.workspace_growths_this_frame << ','
                    << result.ldpc_capacity_growths_this_frame << ','
                    << static_cast<unsigned>(result.synchronization_mode_used) << ','
                    << result.tracking_fallback << ','
                    << result.timing_candidates_evaluated << '\n';
            }
        }

        const double reference_median =
            percentile(variants.front().receiver, 0.50);
        std::cout << std::fixed << std::setprecision(2)
                  << "Frames per variant: " << frames << '\n';
        for (const auto& variant : variants) {
            const double median = percentile(variant.receiver, 0.50);
            std::cout << std::setw(14) << variant.name
                      << " receiver mean/median/p95/p99 "
                      << mean(variant.receiver) << '/' << median << '/'
                      << percentile(variant.receiver, 0.95) << '/'
                      << percentile(variant.receiver, 0.99) << " us; speedup "
                      << reference_median / median << "x; deadline ratio "
                      << median / 225.0 << "x; pipeline median/p99 "
                      << percentile(variant.pipeline_interval, 0.50) << '/'
                      << percentile(variant.pipeline_interval, 0.99)
                      << " us\n";
        }
        std::cout << "All variants CRC-compatible; post-warm-up growths 0\n"
                  << "Wrote " << output.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
