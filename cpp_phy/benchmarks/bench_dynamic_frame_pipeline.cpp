#include "openisac/binary_io.hpp"
#include "openisac/dynamic_frame.hpp"
#include "openisac/dynamic_frame_pipeline.hpp"
#include "openisac/frame.hpp"
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

struct InputFrame {
    std::vector<float> control_llrs;
    std::vector<std::complex<float>> payload_symbols;
    std::vector<float> variances;
    std::vector<std::uint8_t> expected_payload;
};

InputFrame build_input(
    std::size_t frame,
    const openisac::LinkMode& mode,
    const openisac::FormalFrameLayout& layout,
    const openisac::Ldpc5041008& codec) {
    InputFrame input;
    input.expected_payload.resize(layout.user_payload_bytes);
    for (std::size_t index = 0u; index < input.expected_payload.size(); ++index) {
        input.expected_payload[index] = static_cast<std::uint8_t>(
            (index * 37u + frame * 11u + 5u) & 0xFFu);
    }
    const auto encoded = openisac::encode_dynamic_frame(
        input.expected_payload, mode, static_cast<std::uint16_t>(frame), codec,
        static_cast<std::uint32_t>(0xB100u + frame * 13u));
    input.control_llrs.reserve(encoded.control_labels.size() * 2u);
    for (const auto label : encoded.control_labels) {
        input.control_llrs.push_back((label & 0x02u) == 0u ? 12.0f : -12.0f);
        input.control_llrs.push_back((label & 0x01u) == 0u ? 12.0f : -12.0f);
    }
    input.payload_symbols = encoded.payload_symbols;
    input.variances.assign(input.payload_symbols.size(), 0.01f);
    return input;
}

void verify(
    std::uint64_t frame_id,
    const openisac::DecodedDynamicFrame& decoded,
    const std::vector<InputFrame>& inputs,
    const openisac::LinkMode& mode) {
    if (frame_id >= inputs.size() || !decoded.crc_ok || decoded.mode != mode ||
        decoded.header.sequence != frame_id ||
        decoded.user_payload != inputs[static_cast<std::size_t>(frame_id)].expected_payload) {
        throw std::runtime_error("pipeline benchmark changed decoded frame content");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path(
                  "measurement/cpp_pipeline_benchmark/frames.csv");
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
        openisac::FormalFrameProfile profile;
        profile.transmit_rank = mode.rank;
        profile.bits_per_symbol = openisac::modulation_bits(mode.modulation);
        const auto layout = openisac::build_formal_frame_layout(profile);

        std::vector<InputFrame> inputs;
        inputs.reserve(frames);
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            inputs.push_back(build_input(frame, mode, layout, codec));
        }

        openisac::DynamicFrameDecodeWorkspace serial_workspace;
        openisac::LdpcFrameDecoder serial_decoder(codec, 8u);
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            const auto& input = inputs[warm];
            const auto decoded = openisac::decode_dynamic_frame_llrs(
                input.control_llrs, input.payload_symbols, input.variances,
                codec, 6u, 0.8f, &serial_decoder, &serial_workspace);
            verify(warm, decoded, inputs, mode);
        }
        std::vector<double> serial_wall;
        std::vector<double> serial_producer;
        std::vector<double> serial_fec;
        serial_wall.reserve(frames);
        serial_producer.reserve(frames);
        serial_fec.reserve(frames);
        const auto serial_start = Clock::now();
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            const auto begin = Clock::now();
            const auto& input = inputs[frame];
            const auto decoded = openisac::decode_dynamic_frame_llrs(
                input.control_llrs, input.payload_symbols, input.variances,
                codec, 6u, 0.8f, &serial_decoder, &serial_workspace);
            const auto done = Clock::now();
            verify(frame, decoded, inputs, mode);
            if (decoded.ldpc_capacity_growths_this_frame != 0u) {
                throw std::runtime_error("warmed serial LDPC buffers grew");
            }
            serial_wall.push_back(elapsed_us(begin, done));
            serial_producer.push_back(
                elapsed_us(begin, done) - decoded.ldpc_crc_us);
            serial_fec.push_back(decoded.ldpc_crc_us);
        }
        const double serial_total_us = elapsed_us(serial_start, Clock::now());

        openisac::DynamicFramePipeline pipeline(codec, 8u);
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            const auto& input = inputs[warm];
            pipeline.submit(
                warm, input.control_llrs, input.payload_symbols, input.variances);
        }
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            const auto output_frame = pipeline.receive();
            verify(output_frame.frame_id, output_frame.decoded, inputs, mode);
        }

        std::vector<openisac::DynamicFramePipelineResult> pipeline_results;
        pipeline_results.reserve(frames);
        const auto pipeline_start = Clock::now();
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            if (frame >= pipeline.slot_count()) {
                pipeline_results.push_back(pipeline.receive());
            }
            const auto& input = inputs[frame];
            pipeline.submit(
                frame, input.control_llrs,
                input.payload_symbols, input.variances);
        }
        while (pipeline_results.size() < frames) {
            pipeline_results.push_back(pipeline.receive());
        }
        const double pipeline_total_us = elapsed_us(pipeline_start, Clock::now());

        std::vector<double> pipeline_producer;
        std::vector<double> pipeline_queue;
        std::vector<double> pipeline_fec;
        std::vector<double> pipeline_latency;
        pipeline_producer.reserve(frames);
        pipeline_queue.reserve(frames);
        pipeline_fec.reserve(frames);
        pipeline_latency.reserve(frames);
        std::ofstream csv(output);
        if (!csv) {
            throw std::runtime_error("cannot write " + output.string());
        }
        csv << "frame,serial_wall_us,serial_producer_us,serial_fec_us,"
               "pipeline_producer_us,pipeline_queue_wait_us,pipeline_fec_us,"
               "pipeline_latency_us,buffer_slot,crc_ok,prepared_growths,"
               "ldpc_growths\n";
        csv << std::setprecision(10);
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            const auto& output_frame = pipeline_results[frame];
            verify(output_frame.frame_id, output_frame.decoded, inputs, mode);
            if (output_frame.timing.capacity_growths_this_frame != 0u ||
                output_frame.decoded.ldpc_capacity_growths_this_frame != 0u) {
                throw std::runtime_error("warmed pipeline buffers grew");
            }
            pipeline_producer.push_back(output_frame.timing.producer_us);
            pipeline_queue.push_back(output_frame.timing.queue_wait_us);
            pipeline_fec.push_back(output_frame.timing.fec_us);
            pipeline_latency.push_back(output_frame.timing.latency_us);
            csv << frame << ',' << serial_wall[frame] << ','
                << serial_producer[frame] << ',' << serial_fec[frame] << ','
                << output_frame.timing.producer_us << ','
                << output_frame.timing.queue_wait_us << ','
                << output_frame.timing.fec_us << ','
                << output_frame.timing.latency_us << ','
                << output_frame.timing.buffer_slot << ','
                << output_frame.decoded.crc_ok << ','
                << output_frame.timing.capacity_growths_this_frame << ','
                << output_frame.decoded.ldpc_capacity_growths_this_frame << '\n';
        }

        const double serial_interval_us = serial_total_us / frames;
        const double pipeline_interval_us = pipeline_total_us / frames;
        const auto summary_path = output.parent_path() / "summary.csv";
        std::ofstream summary(summary_path);
        if (!summary) {
            throw std::runtime_error("cannot write " + summary_path.string());
        }
        summary << "frames,buffers,ldpc_workers,serial_interval_us,"
                   "pipeline_interval_us,pipeline_throughput_fps,speedup,"
                   "producer_mean_us,producer_median_us,producer_p95_us,"
                   "producer_p99_us,fec_mean_us,fec_median_us,fec_p95_us,"
                   "fec_p99_us,queue_median_us,queue_p95_us,queue_p99_us,"
                   "latency_median_us,latency_p95_us,latency_p99_us,"
                   "crc_success_rate,post_warmup_growths\n";
        summary << std::setprecision(10)
                << frames << ",2,8," << serial_interval_us << ','
                << pipeline_interval_us << ',' << 1.0e6 / pipeline_interval_us
                << ',' << serial_interval_us / pipeline_interval_us << ','
                << mean(pipeline_producer) << ','
                << percentile(pipeline_producer, 0.50) << ','
                << percentile(pipeline_producer, 0.95) << ','
                << percentile(pipeline_producer, 0.99) << ','
                << mean(pipeline_fec) << ','
                << percentile(pipeline_fec, 0.50) << ','
                << percentile(pipeline_fec, 0.95) << ','
                << percentile(pipeline_fec, 0.99) << ','
                << percentile(pipeline_queue, 0.50) << ','
                << percentile(pipeline_queue, 0.95) << ','
                << percentile(pipeline_queue, 0.99) << ','
                << percentile(pipeline_latency, 0.50) << ','
                << percentile(pipeline_latency, 0.95) << ','
                << percentile(pipeline_latency, 0.99)
                << ",1,0\n";
        std::cout << std::fixed << std::setprecision(2)
                  << "Frames: " << frames << "; two buffers; 8 LDPC workers\n"
                  << "Serial actual interval: " << serial_interval_us
                  << " us/frame\n"
                  << "Pipeline actual interval: " << pipeline_interval_us
                  << " us/frame; speedup "
                  << serial_interval_us / pipeline_interval_us << "x\n"
                  << "Pipeline producer mean/median/p99: "
                  << mean(pipeline_producer) << '/'
                  << percentile(pipeline_producer, 0.50) << '/'
                  << percentile(pipeline_producer, 0.99) << " us\n"
                  << "Pipeline FEC mean/median/p99: "
                  << mean(pipeline_fec) << '/'
                  << percentile(pipeline_fec, 0.50) << '/'
                  << percentile(pipeline_fec, 0.99) << " us\n"
                  << "Queue wait median/p99: "
                  << percentile(pipeline_queue, 0.50) << '/'
                  << percentile(pipeline_queue, 0.99) << " us\n"
                  << "Latency median/p99: "
                  << percentile(pipeline_latency, 0.50) << '/'
                  << percentile(pipeline_latency, 0.99) << " us\n"
                  << "CRC-compatible; post-warm-up growths 0\n"
                  << "Wrote " << output.string() << " and "
                  << summary_path.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
