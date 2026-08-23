#include "openisac/binary_io.hpp"
#include "openisac/dynamic_link.hpp"
#include "openisac/dynamic_link_pipeline.hpp"
#include "openisac/ldpc.hpp"
#include "openisac/ldpc_frame_decoder.hpp"

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
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_us(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}

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

openisac::DynamicLinkSimulationConfig frame_config(std::size_t frame) {
    openisac::DynamicLinkSimulationConfig config;
    config.snr_db = 45.0f;
    config.timing_offset_samples = 20u;
    config.cfo_hz = 300.0f;
    config.sfo_ppm = 20.0f;
    config.csi_smoothing_alpha = 0.35f;
    config.random_seed = static_cast<unsigned>(0xC100u + frame * 29u);
    return config;
}

void verify(const openisac::DynamicLinkSimulationResult& result) {
    if (!result.timing_ok || !result.header_ok || !result.crc_ok ||
        result.decoded_mode != openisac::LinkMode{
            2u, openisac::Modulation::qam64}) {
        throw std::runtime_error("complete-link benchmark lost a frame");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path(
                  "measurement/cpp_full_pipeline_benchmark/frames.csv");
        const std::size_t frames = argc > 2
            ? static_cast<std::size_t>(std::stoul(argv[2]))
            : 1000u;
        if (frames < 20u || frames > 65535u) {
            throw std::invalid_argument("frame count must be in [20,65535]");
        }
        std::filesystem::create_directories(output.parent_path());
        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        const openisac::LinkMode mode{2u, openisac::Modulation::qam64};

        openisac::DynamicLinkReceiverState serial_state;
        openisac::DynamicLinkWorkspace serial_workspace;
        openisac::LdpcFrameDecoder serial_decoder(codec, 8u);
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            const auto result = openisac::simulate_dynamic_tdl_frame(
                mode, static_cast<std::uint16_t>(60000u + warm), codec,
                frame_config(warm), &serial_state, &serial_workspace,
                &serial_decoder);
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
            const auto result = openisac::simulate_dynamic_tdl_frame(
                mode, static_cast<std::uint16_t>(frame), codec,
                frame_config(frame), &serial_state, &serial_workspace,
                &serial_decoder);
            const auto done = Clock::now();
            verify(result);
            if (result.workspace_growths_this_frame != 0u ||
                result.ldpc_capacity_growths_this_frame != 0u) {
                throw std::runtime_error("warmed serial buffers grew");
            }
            serial_wall.push_back(elapsed_us(begin, done));
            serial_receiver.push_back(result.timing.receiver_total_us);
            serial_front.push_back(
                result.timing.receiver_total_us - result.timing.ldpc_crc_us);
            serial_fec.push_back(result.timing.ldpc_crc_us);
        }
        const double serial_total_us = elapsed_us(serial_start, Clock::now());

        openisac::DynamicLinkPipeline pipeline(codec, 8u);
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            pipeline.submit(
                warm, mode, static_cast<std::uint16_t>(60000u + warm),
                frame_config(warm));
        }
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            verify(pipeline.receive().link);
        }

        std::vector<openisac::DynamicLinkPipelineResult> pipeline_results;
        pipeline_results.reserve(frames);
        const auto pipeline_start = Clock::now();
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            if (frame >= pipeline.slot_count()) {
                pipeline_results.push_back(pipeline.receive());
            }
            pipeline.submit(
                frame, mode, static_cast<std::uint16_t>(frame),
                frame_config(frame));
        }
        while (pipeline_results.size() < frames) {
            pipeline_results.push_back(pipeline.receive());
        }
        const double pipeline_total_us = elapsed_us(pipeline_start, Clock::now());

        std::vector<double> producer;
        std::vector<double> receiver_front;
        std::vector<double> queue;
        std::vector<double> fec;
        std::vector<double> latency;
        producer.reserve(frames);
        receiver_front.reserve(frames);
        queue.reserve(frames);
        fec.reserve(frames);
        latency.reserve(frames);
        std::ofstream csv(output);
        if (!csv) {
            throw std::runtime_error("cannot write " + output.string());
        }
        csv << "frame,serial_simulator_wall_us,serial_receiver_total_us,"
               "serial_receiver_front_us,serial_fec_us,"
               "pipeline_producer_wall_us,pipeline_receiver_front_us,"
               "pipeline_queue_wait_us,pipeline_fec_wall_us,"
               "pipeline_latency_us,buffer_slot,crc_ok,workspace_growths,"
               "ldpc_growths\n";
        csv << std::setprecision(10);
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            const auto& item = pipeline_results[frame];
            verify(item.link);
            if (item.frame_id != frame ||
                item.link.workspace_growths_this_frame != 0u ||
                item.link.ldpc_capacity_growths_this_frame != 0u) {
                throw std::runtime_error(
                    "warmed complete-link pipeline order/growth failure");
            }
            producer.push_back(item.timing.producer_wall_us);
            receiver_front.push_back(item.timing.receiver_front_us);
            queue.push_back(item.timing.queue_wait_us);
            fec.push_back(item.timing.fec_wall_us);
            latency.push_back(item.timing.latency_us);
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

        const double serial_interval = serial_total_us / frames;
        const double pipeline_interval = pipeline_total_us / frames;
        const auto summary_path = output.parent_path() / "summary.csv";
        std::ofstream summary(summary_path);
        if (!summary) {
            throw std::runtime_error("cannot write " + summary_path.string());
        }
        summary << "frames,buffers,ldpc_workers,serial_simulator_interval_us,"
                   "pipeline_simulator_interval_us,pipeline_frames_per_second,"
                   "simulator_speedup,receiver_front_mean_us,fec_mean_us,"
                   "queue_median_us,queue_p95_us,queue_p99_us,"
                   "latency_median_us,latency_p95_us,latency_p99_us,"
                   "crc_success_rate,post_warmup_growths\n";
        summary << std::setprecision(10) << frames << ",2,8,"
                << serial_interval << ',' << pipeline_interval << ','
                << 1.0e6 / pipeline_interval << ','
                << serial_interval / pipeline_interval << ','
                << mean(receiver_front) << ',' << mean(fec) << ','
                << percentile(queue, 0.50) << ','
                << percentile(queue, 0.95) << ','
                << percentile(queue, 0.99) << ','
                << percentile(latency, 0.50) << ','
                << percentile(latency, 0.95) << ','
                << percentile(latency, 0.99) << ",1,0\n";

        std::cout << std::fixed << std::setprecision(2)
                  << "Frames: " << frames
                  << "; complete simulator; two buffers; 8 LDPC workers\n"
                  << "Serial simulator interval: " << serial_interval
                  << " us/frame\n"
                  << "Pipeline simulator interval: " << pipeline_interval
                  << " us/frame; " << 1.0e6 / pipeline_interval
                  << " frame/s; speedup "
                  << serial_interval / pipeline_interval << "x\n"
                  << "Receiver front mean/median/p99: "
                  << mean(receiver_front) << '/'
                  << percentile(receiver_front, 0.50) << '/'
                  << percentile(receiver_front, 0.99) << " us\n"
                  << "FEC mean/median/p99: " << mean(fec) << '/'
                  << percentile(fec, 0.50) << '/'
                  << percentile(fec, 0.99) << " us\n"
                  << "Queue wait median/p99: "
                  << percentile(queue, 0.50) << '/'
                  << percentile(queue, 0.99) << " us\n"
                  << "End-to-end latency median/p99: "
                  << percentile(latency, 0.50) << '/'
                  << percentile(latency, 0.99) << " us\n"
                  << "CRC success 100%; post-warm-up growths 0\n"
                  << "Note: simulator interval includes transmitter and channel "
                     "generation; receiver_front_us excludes those stages.\n"
                  << "Wrote " << output.string() << " and "
                  << summary_path.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
