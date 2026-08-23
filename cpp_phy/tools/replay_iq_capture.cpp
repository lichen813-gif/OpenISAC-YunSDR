#include "openisac/binary_io.hpp"
#include "openisac/capture_io.hpp"
#include "openisac/dynamic_link_pipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 4) {
            std::cerr << "Usage: openisac_phy_replay_iq <capture.oiq> "
                         "[ldpc_workers=8] [max_samples=4096]\n";
            return 2;
        }
        const std::size_t ldpc_workers = argc >= 3
            ? static_cast<std::size_t>(std::stoull(argv[2]))
            : 8u;
        const std::size_t maximum_samples = argc >= 4
            ? static_cast<std::size_t>(std::stoull(argv[3]))
            : 4096u;
        if (ldpc_workers == 0u || maximum_samples == 0u) {
            throw std::invalid_argument("worker and sample limits must be non-zero");
        }

        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        openisac::IqCaptureFileReader reader(argv[1], maximum_samples);
        if (reader.antenna_count() != 2u) {
            throw std::runtime_error(
                "the current dynamic receiver requires exactly two IQ branches");
        }
        openisac::CaptureRingBuffer ring(
            8u, reader.antenna_count(), maximum_samples);
        openisac::DynamicLinkReceiverConfig receiver_config;
        openisac::DynamicLinkPipeline pipeline(codec, ldpc_workers);
        openisac::DynamicLinkCaptureFrame input;
        openisac::DynamicLinkCaptureFrame scratch;

        std::uint64_t submitted = 0u;
        std::uint64_t received = 0u;
        std::uint64_t timing_passes = 0u;
        std::uint64_t header_passes = 0u;
        std::uint64_t crc_passes = 0u;
        auto collect = [&] {
            const auto result = pipeline.receive();
            timing_passes += result.link.timing_ok ? 1u : 0u;
            header_passes += result.link.header_ok ? 1u : 0u;
            crc_passes += result.link.crc_ok ? 1u : 0u;
            ++received;
        };

        while (reader.read_next(input)) {
            if (submitted - received >= pipeline.slot_count()) {
                collect();
            }
            if (!ring.try_push(input)) {
                continue;
            }
            if (!openisac::submit_next_capture(
                    ring, pipeline, receiver_config, scratch)) {
                throw std::runtime_error("capture ring became empty before submit");
            }
            ++submitted;
        }
        while (received < submitted) {
            collect();
        }

        const auto stats = ring.statistics();
        const double crc_percent = received == 0u ? 0.0 :
            100.0 * static_cast<double>(crc_passes) /
                static_cast<double>(received);
        std::cout << std::fixed << std::setprecision(2)
                  << "Frames read/submitted/received: " << reader.frames_read()
                  << '/' << submitted << '/' << received << '\n'
                  << "Timing/header/CRC passes: " << timing_passes << '/'
                  << header_passes << '/' << crc_passes << " ("
                  << crc_percent << "% CRC)\n"
                  << "Source gaps/out-of-order/timestamp regressions: "
                  << stats.source_sequence_gaps << '/'
                  << stats.out_of_order_frames << '/'
                  << stats.timestamp_regressions << '\n'
                  << "Ring overflow/consumer gaps/high watermark: "
                  << stats.overflow_drops << '/'
                  << stats.consumer_sequence_gaps << '/'
                  << stats.high_watermark << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "IQ replay failed: " << error.what() << '\n';
        return 1;
    }
}
