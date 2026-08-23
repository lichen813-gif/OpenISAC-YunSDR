#include "openisac/binary_io.hpp"
#include "openisac/capture_io.hpp"
#include "openisac/dynamic_link.hpp"
#include "openisac/dynamic_link_pipeline.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_us(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

struct FileCleanup {
    std::filesystem::path path;
    ~FileCleanup() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::size_t pipeline_frames = argc > 1
            ? static_cast<std::size_t>(std::stoull(argv[1]))
            : 10000u;
        if (pipeline_frames < 2u) {
            throw std::invalid_argument("pipeline frame count must be at least two");
        }
        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        openisac::DynamicLinkSimulationConfig simulation;
        simulation.snr_db = 45.0f;
        simulation.enable_truth_diagnostics = false;
        simulation.random_seed = 0xCA71u;
        openisac::DynamicLinkWorkspace generation_workspace;
        openisac::DynamicLinkIqFrame generated;
        openisac::generate_dynamic_tdl_iq_frame(
            {2u, openisac::Modulation::qam64}, 7u, codec, simulation,
            generated, generation_workspace);
        const std::size_t antennas = generated.samples.size();
        const std::size_t samples = generated.samples.front().size();

        constexpr std::size_t ring_iterations = 20000u;
        openisac::CaptureRingBuffer copy_ring(8u, antennas, samples);
        openisac::DynamicLinkCaptureFrame copied;
        const auto ring_start = Clock::now();
        for (std::size_t index = 0u; index < ring_iterations; ++index) {
            generated.capture_sequence = index;
            generated.timestamp = 225000u * index;
            if (!copy_ring.try_push(generated) || !copy_ring.try_pop(copied)) {
                throw std::runtime_error("ring benchmark unexpectedly dropped a frame");
            }
        }
        const double ring_total_us = elapsed_us(ring_start, Clock::now());
        const double copied_bytes = static_cast<double>(ring_iterations) * 2.0 *
            static_cast<double>(antennas) * static_cast<double>(samples) *
            sizeof(std::complex<float>);
        const double ring_gib_per_second = copied_bytes /
            (ring_total_us * 1.0e-6) / (1024.0 * 1024.0 * 1024.0);

        constexpr std::size_t file_frames = 256u;
        const auto file_path = std::filesystem::temp_directory_path() /
            "openisac_capture_io_benchmark.oiq";
        FileCleanup cleanup{file_path};
        const auto write_start = Clock::now();
        {
            openisac::IqCaptureFileWriter writer(file_path.string(), antennas);
            for (std::size_t index = 0u; index < file_frames; ++index) {
                generated.capture_sequence = index;
                generated.timestamp = 225000u * index;
                writer.append(generated);
            }
            writer.flush();
        }
        const double write_us = elapsed_us(write_start, Clock::now());
        const double file_mebibytes = static_cast<double>(
            std::filesystem::file_size(file_path)) / (1024.0 * 1024.0);
        std::size_t file_frames_read = 0u;
        const auto read_start = Clock::now();
        {
            openisac::IqCaptureFileReader reader(file_path.string(), samples);
            while (reader.read_next(copied)) {
                ++file_frames_read;
            }
        }
        const double read_us = elapsed_us(read_start, Clock::now());
        if (file_frames_read != file_frames) {
            throw std::runtime_error("file benchmark lost an IQ frame");
        }

        const auto receiver_config = openisac::make_dynamic_link_receiver_config(
            simulation, openisac::NoiseVarianceMode::pilot_residual);
        openisac::CaptureRingBuffer pipeline_ring(8u, antennas, samples);
        openisac::DynamicLinkPipeline pipeline(codec, 8u);
        openisac::DynamicLinkCaptureFrame pipeline_scratch;
        std::size_t results_received = 0u;
        std::size_t crc_passes = 0u;
        std::size_t workspace_growths = 0u;
        const auto pipeline_start = Clock::now();
        for (std::size_t index = 0u; index < pipeline_frames; ++index) {
            if (index >= pipeline.slot_count()) {
                const auto result = pipeline.receive();
                if (result.capture_sequence != results_received ||
                    result.capture_timestamp != 225000u * results_received) {
                    throw std::runtime_error("pipeline changed capture metadata order");
                }
                crc_passes += result.link.crc_ok ? 1u : 0u;
                if (results_received >= pipeline.slot_count()) {
                    workspace_growths +=
                        result.link.workspace_growths_this_frame;
                }
                ++results_received;
            }
            generated.capture_sequence = index;
            generated.timestamp = 225000u * index;
            if (!pipeline_ring.try_push(generated) ||
                !openisac::submit_next_capture(
                    pipeline_ring, pipeline, receiver_config,
                    pipeline_scratch)) {
                throw std::runtime_error("capture pipeline dropped a frame");
            }
        }
        while (results_received < pipeline_frames) {
            const auto result = pipeline.receive();
            if (result.capture_sequence != results_received ||
                result.capture_timestamp != 225000u * results_received) {
                throw std::runtime_error("pipeline changed final metadata order");
            }
            crc_passes += result.link.crc_ok ? 1u : 0u;
            if (results_received >= pipeline.slot_count()) {
                workspace_growths += result.link.workspace_growths_this_frame;
            }
            ++results_received;
        }
        const double pipeline_total_us = elapsed_us(
            pipeline_start, Clock::now());
        const auto stats = pipeline_ring.statistics();

        std::cout << std::fixed << std::setprecision(2)
                  << "Capture samples/antenna: " << samples << '\n'
                  << "Ring copy: " << ring_total_us / ring_iterations
                  << " us/frame, " << ring_gib_per_second << " GiB/s\n"
                  << "IQ file: " << file_mebibytes << " MiB, write "
                  << file_mebibytes / (write_us * 1.0e-6) << " MiB/s, read "
                  << file_mebibytes / (read_us * 1.0e-6) << " MiB/s\n"
                  << "Capture pipeline: " << pipeline_total_us / pipeline_frames
                  << " us/frame, "
                  << pipeline_frames / (pipeline_total_us * 1.0e-6)
                  << " frame/s\n"
                  << "CRC: " << crc_passes << '/' << pipeline_frames
                  << ", overflows: " << stats.overflow_drops
                  << ", sequence gaps: " << stats.consumer_sequence_gaps
                  << ", high watermark: " << stats.high_watermark
                  << ", post-warmup workspace growths: "
                  << workspace_growths << '\n';
        return crc_passes == pipeline_frames &&
            stats.overflow_drops == 0u &&
            stats.consumer_sequence_gaps == 0u ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Capture benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
