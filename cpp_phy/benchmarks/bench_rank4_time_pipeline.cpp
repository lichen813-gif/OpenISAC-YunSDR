#include "openisac/binary_io.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/ldpc_frame_decoder.hpp"
#include "openisac/rank4_time_link.hpp"
#include "openisac/rank4_time_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_us(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

double mean(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        throw std::invalid_argument("percentile requires samples");
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1u));
    return values[index];
}

openisac::Rank4TimeSimulationConfig frame_config(std::size_t frame) {
    openisac::Rank4TimeSimulationConfig config;
    config.snr_db = 50.0f;
    config.modulation = openisac::Modulation::qam64;
    config.timing_offset_samples = 20u;
    config.cfo_hz = 300.0f;
    config.sfo_ppm = 20.0f;
    config.transmit_correlation = 0.2f;
    config.receive_correlation = 0.2f;
    config.csi_smoothing_alpha = 1.0f;
    config.random_seed = static_cast<std::uint32_t>(
        0x9100u + frame * 29u);
    return config;
}

void verify(const openisac::Rank4TimeSimulationResult& result) {
    if (!result.timing_ok || !result.header_ok || !result.crc_ok ||
        !result.payload_match || result.syndrome_failures != 0u) {
        throw std::runtime_error("Rank-4 pipeline benchmark lost a frame");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path(
                  "measurement/cpp_4x4_time_pipeline/frames.csv");
        const std::size_t frames = argc > 2
            ? static_cast<std::size_t>(std::stoull(argv[2]))
            : 200u;
        const std::size_t workers = argc > 3
            ? static_cast<std::size_t>(std::stoull(argv[3]))
            : 12u;
        if (frames < 20u || frames > 65535u) {
            throw std::invalid_argument("frame count must be in [20,65535]");
        }
        std::filesystem::create_directories(output.parent_path());
        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));

        openisac::Rank4TimeReceiverState serial_state;
        openisac::Rank4TimeWorkspace serial_workspace;
        openisac::LdpcFrameDecoder serial_decoder(codec, workers);
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            const auto result = openisac::simulate_rank4_time_frame(
                static_cast<std::uint16_t>(60000u + warm),
                frame_config(warm), codec, &serial_state,
                &serial_workspace, &serial_decoder);
            verify(result);
        }

        std::vector<double> serial_wall;
        std::vector<double> serial_receiver;
        std::vector<double> serial_front;
        std::vector<double> serial_fec;
        serial_wall.reserve(frames);
        serial_receiver.reserve(frames);
        serial_front.reserve(frames);
        serial_fec.reserve(frames);
        const auto serial_start = Clock::now();
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            const auto begin = Clock::now();
            const auto result = openisac::simulate_rank4_time_frame(
                static_cast<std::uint16_t>(frame), frame_config(frame), codec,
                &serial_state, &serial_workspace, &serial_decoder);
            const auto done = Clock::now();
            verify(result);
            if (result.workspace_growths_this_frame != 0u ||
                result.ldpc_capacity_growths_this_frame != 0u) {
                throw std::runtime_error("warmed serial Rank-4 buffers grew");
            }
            serial_wall.push_back(elapsed_us(begin, done));
            serial_receiver.push_back(result.receiver_us);
            serial_front.push_back(result.receiver_us - result.ldpc_crc_us);
            serial_fec.push_back(result.ldpc_crc_us);
        }
        const double serial_total_us = elapsed_us(serial_start, Clock::now());

        openisac::Rank4TimePipeline pipeline(codec, workers);
        for (std::size_t warm = 0u; warm < pipeline.slot_count(); ++warm) {
            pipeline.submit(
                warm, static_cast<std::uint16_t>(61000u + warm),
                frame_config(warm));
        }
        for (std::size_t warm = 0u; warm < pipeline.slot_count(); ++warm) {
            verify(pipeline.receive().link);
        }

        std::vector<openisac::Rank4TimePipelineResult> results;
        results.reserve(frames);
        const auto pipeline_start = Clock::now();
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            if (frame >= pipeline.slot_count()) {
                results.push_back(pipeline.receive());
            }
            pipeline.submit(
                frame, static_cast<std::uint16_t>(frame),
                frame_config(frame));
        }
        while (results.size() < frames) {
            results.push_back(pipeline.receive());
        }
        const double pipeline_total_us = elapsed_us(
            pipeline_start, Clock::now());

        std::vector<double> producer;
        std::vector<double> receiver_front;
        std::vector<double> queue;
        std::vector<double> fec;
        std::vector<double> latency;
        std::vector<double> ideal_service;
        producer.reserve(frames);
        receiver_front.reserve(frames);
        queue.reserve(frames);
        fec.reserve(frames);
        latency.reserve(frames);
        ideal_service.reserve(frames);
        std::ofstream csv(output);
        if (!csv) {
            throw std::runtime_error("cannot write " + output.string());
        }
        csv << "frame,serial_simulator_wall_us,serial_receiver_us,"
               "serial_front_us,serial_fec_us,pipeline_producer_wall_us,"
               "pipeline_receiver_front_us,pipeline_queue_wait_us,"
               "pipeline_fec_wall_us,pipeline_latency_us,buffer_slot,crc_ok,"
               "workspace_growths,ldpc_growths\n";
        csv << std::setprecision(10);
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            const auto& item = results[frame];
            verify(item.link);
            if (item.frame_id != frame ||
                item.link.workspace_growths_this_frame != 0u ||
                item.link.ldpc_capacity_growths_this_frame != 0u) {
                throw std::runtime_error(
                    "warmed Rank-4 pipeline order/growth failure");
            }
            producer.push_back(item.timing.producer_wall_us);
            receiver_front.push_back(item.timing.receiver_front_us);
            queue.push_back(item.timing.queue_wait_us);
            fec.push_back(item.timing.fec_wall_us);
            latency.push_back(item.timing.latency_us);
            ideal_service.push_back(std::max(
                item.timing.receiver_front_us, item.timing.fec_wall_us));
            csv << frame << ',' << serial_wall[frame] << ','
                << serial_receiver[frame] << ',' << serial_front[frame] << ','
                << serial_fec[frame] << ',' << item.timing.producer_wall_us
                << ',' << item.timing.receiver_front_us << ','
                << item.timing.queue_wait_us << ',' << item.timing.fec_wall_us
                << ',' << item.timing.latency_us << ','
                << item.timing.buffer_slot << ',' << item.link.crc_ok << ','
                << item.link.workspace_growths_this_frame << ','
                << item.link.ldpc_capacity_growths_this_frame << '\n';
        }

        const double serial_interval = serial_total_us /
            static_cast<double>(frames);
        const double pipeline_interval = pipeline_total_us /
            static_cast<double>(frames);
        const auto summary_path = output.parent_path() / "summary.csv";
        std::ofstream summary(summary_path);
        if (!summary) {
            throw std::runtime_error("cannot write " + summary_path.string());
        }
        summary << "metric,value\n" << std::setprecision(12)
                << "frames," << frames << '\n'
                << "buffers,2\n"
                << "ldpc_workers," << workers << '\n'
                << "serial_simulator_interval_us," << serial_interval << '\n'
                << "pipeline_simulator_interval_us," << pipeline_interval << '\n'
                << "pipeline_simulator_speedup,"
                << serial_interval / pipeline_interval << '\n'
                << "serial_receiver_mean_us," << mean(serial_receiver) << '\n'
                << "receiver_front_mean_us," << mean(receiver_front) << '\n'
                << "receiver_front_p50_us,"
                << percentile(receiver_front, 0.50) << '\n'
                << "receiver_front_p99_us,"
                << percentile(receiver_front, 0.99) << '\n'
                << "fec_mean_us," << mean(fec) << '\n'
                << "fec_p50_us," << percentile(fec, 0.50) << '\n'
                << "fec_p99_us," << percentile(fec, 0.99) << '\n'
                << "ideal_receiver_pipeline_mean_us,"
                << mean(ideal_service) << '\n'
                << "queue_p50_us," << percentile(queue, 0.50) << '\n'
                << "queue_p99_us," << percentile(queue, 0.99) << '\n'
                << "latency_p50_us," << percentile(latency, 0.50) << '\n'
                << "latency_p99_us," << percentile(latency, 0.99) << '\n'
                << "crc_success_rate,1\n"
                << "post_warmup_growths,0\n";

        std::cout << std::fixed << std::setprecision(2)
                  << "Frames: " << frames << "; two slots; " << workers
                  << " LDPC workers\n"
                  << "Serial receiver mean: " << mean(serial_receiver)
                  << " us\n"
                  << "Front mean/P50/P99: " << mean(receiver_front) << '/'
                  << percentile(receiver_front, 0.50) << '/'
                  << percentile(receiver_front, 0.99) << " us\n"
                  << "FEC mean/P50/P99: " << mean(fec) << '/'
                  << percentile(fec, 0.50) << '/'
                  << percentile(fec, 0.99) << " us\n"
                  << "Ideal capture-input pipeline service mean: "
                  << mean(ideal_service) << " us/frame\n"
                  << "Actual simulator serial/pipeline interval: "
                  << serial_interval << '/' << pipeline_interval
                  << " us; speedup " << serial_interval / pipeline_interval
                  << "x\n"
                  << "Latency P50/P99: " << percentile(latency, 0.50)
                  << '/' << percentile(latency, 0.99) << " us\n"
                  << "CRC success 100%; post-warm-up growths 0\n"
                  << "Note: actual interval includes transmitter and TDL "
                     "generation; capture-input service is reported as an "
                     "explicit bound until a 4-Rx IQ API is added.\n"
                  << "Wrote " << output.string() << " and "
                  << summary_path.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
