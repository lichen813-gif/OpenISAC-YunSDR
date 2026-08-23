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

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

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

double maximum(const std::vector<double>& values) {
    return *std::max_element(values.begin(), values.end());
}

std::size_t count_over(
    const std::vector<double>& values,
    double threshold) {
    return static_cast<std::size_t>(std::count_if(
        values.begin(), values.end(),
        [threshold](double value) { return value > threshold; }));
}

bool apply_windows_engineering_priority() {
#if defined(_WIN32)
    const bool process_ok =
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != 0;
    const bool thread_ok =
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != 0;
    return process_ok && thread_ok;
#else
    return false;
#endif
}

openisac::DynamicLinkSimulationConfig frame_config(std::size_t frame) {
    openisac::DynamicLinkSimulationConfig config;
    config.snr_db = 45.0f;
    config.timing_offset_samples = 20u;
    config.cfo_hz = 300.0f;
    config.sfo_ppm = 20.0f;
    config.csi_smoothing_alpha = 0.35f;
    config.enable_truth_diagnostics = false;
    config.random_seed = static_cast<unsigned>(0xE100u + frame * 31u);
    return config;
}

void verify(const openisac::DynamicLinkSimulationResult& result) {
    if (!result.timing_ok || !result.header_ok || !result.crc_ok ||
        !result.noise_variance_estimated ||
        result.decoded_mode != openisac::LinkMode{
            2u, openisac::Modulation::qam64}) {
        throw std::runtime_error("receiver-only benchmark lost a frame");
    }
}

openisac::DynamicLinkSimulationResult receive_serial(
    const openisac::DynamicLinkCaptureFrame& capture,
    const openisac::DynamicLinkReceiverConfig& receiver_config,
    const openisac::Ldpc5041008& codec,
    openisac::DynamicLinkReceiverState& state,
    openisac::DynamicLinkWorkspace& workspace,
    openisac::LdpcFrameDecoder& decoder) {
    openisac::PreparedDynamicLinkFrame prepared;
    openisac::prepare_captured_iq_frame(
        capture, receiver_config, prepared, &state, workspace);
    return openisac::finish_dynamic_tdl_frame(
        prepared, codec, workspace, &decoder);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1
            ? std::filesystem::path(argv[1])
            : std::filesystem::path(
                  "measurement/cpp_iq_pipeline_benchmark/frames.csv");
        const std::size_t frames = argc > 2
            ? static_cast<std::size_t>(std::stoul(argv[2]))
            : 1000u;
        if (frames < 20u || frames > 1000000u) {
            throw std::invalid_argument("frame count must be in [20,1000000]");
        }
        const std::size_t corpus_frames = argc > 3
            ? static_cast<std::size_t>(std::stoul(argv[3]))
            : std::min<std::size_t>(frames, 256u);
        if (corpus_frames < 2u || corpus_frames > frames ||
            corpus_frames > 65535u) {
            throw std::invalid_argument(
                "IQ corpus frame count must be in [2,min(frames,65535)]");
        }
        const std::string scheduling_profile = argc > 4
            ? std::string(argv[4])
            : std::string("normal");
        if (scheduling_profile != "normal" &&
            scheduling_profile != "windows-engineering") {
            throw std::invalid_argument(
                "scheduling profile must be normal or windows-engineering");
        }
        bool scheduling_applied = false;
        if (scheduling_profile == "windows-engineering") {
            scheduling_applied = apply_windows_engineering_priority();
            if (!scheduling_applied) {
                throw std::runtime_error(
                    "windows-engineering scheduling is unavailable");
            }
        }
        std::filesystem::create_directories(output.parent_path());
        const std::string matrix_root = OPENISAC_MATRIX_DIR;
        const openisac::Ldpc5041008 codec(
            openisac::join_path(matrix_root, "LDPC_504_1008.alist"),
            openisac::join_path(matrix_root, "LDPC_504_1008G.alist"));
        const openisac::LinkMode mode{2u, openisac::Modulation::qam64};
        const auto receiver_config = openisac::make_dynamic_link_receiver_config(
            frame_config(0u), openisac::NoiseVarianceMode::pilot_residual);

        // Generate/capture all IQ before either timed receiver path starts.
        openisac::DynamicLinkWorkspace generation_workspace;
        std::vector<openisac::DynamicLinkIqFrame> inputs(corpus_frames);
        const auto generation_start = Clock::now();
        for (std::size_t frame = 0u; frame < corpus_frames; ++frame) {
            openisac::generate_dynamic_tdl_iq_frame(
                mode, static_cast<std::uint16_t>(frame), codec,
                frame_config(frame), inputs[frame], generation_workspace);
        }
        const double generation_total_us =
            elapsed_us(generation_start, Clock::now());

        openisac::DynamicLinkReceiverState serial_state;
        openisac::DynamicLinkWorkspace serial_workspace;
        openisac::LdpcFrameDecoder serial_decoder(codec, 8u);
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            verify(receive_serial(
                inputs[warm], receiver_config, codec, serial_state,
                serial_workspace, serial_decoder));
        }
        std::vector<double> serial_wall;
        std::vector<double> serial_front;
        std::vector<double> serial_fec;
        serial_wall.reserve(frames);
        serial_front.reserve(frames);
        serial_fec.reserve(frames);
        const auto serial_start = Clock::now();
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            const auto begin = Clock::now();
            const auto result = receive_serial(
                inputs[frame % corpus_frames], receiver_config, codec, serial_state,
                serial_workspace, serial_decoder);
            const auto done = Clock::now();
            verify(result);
            if (result.workspace_growths_this_frame != 0u ||
                result.ldpc_capacity_growths_this_frame != 0u) {
                throw std::runtime_error("warmed serial RX buffers grew");
            }
            serial_wall.push_back(elapsed_us(begin, done));
            serial_front.push_back(
                result.timing.receiver_total_us - result.timing.ldpc_crc_us);
            serial_fec.push_back(result.timing.ldpc_crc_us);
        }
        const double serial_total_us = elapsed_us(serial_start, Clock::now());

        openisac::DynamicLinkPipeline pipeline(codec, 8u);
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            pipeline.submit_capture(
                warm, inputs[warm], receiver_config);
        }
        for (std::size_t warm = 0u; warm < 2u; ++warm) {
            verify(pipeline.receive().link);
        }
        std::vector<openisac::DynamicLinkPipelineResult> results;
        results.reserve(frames);
        const auto pipeline_start = Clock::now();
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            if (frame >= pipeline.slot_count()) {
                results.push_back(pipeline.receive());
            }
            pipeline.submit_capture(
                frame, inputs[frame % corpus_frames], receiver_config);
        }
        while (results.size() < frames) {
            results.push_back(pipeline.receive());
        }
        const double pipeline_total_us = elapsed_us(pipeline_start, Clock::now());

        std::vector<double> producer;
        std::vector<double> submit_call;
        std::vector<double> backpressure;
        std::vector<double> receiver_front;
        std::vector<double> queue;
        std::vector<double> fec;
        std::vector<double> latency;
        std::vector<double> synchronization;
        std::vector<double> fft_csi;
        std::vector<double> fft_grid;
        std::vector<double> sfo_correction;
        std::vector<double> noise_estimation;
        std::vector<double> channel_estimation;
        std::vector<double> csi_smoothing;
        std::vector<double> detection;
        std::vector<double> control_header;
        std::vector<double> soft_demapping;
        std::vector<double> raw_noise_variance;
        std::vector<double> used_noise_variance;
        producer.reserve(frames);
        submit_call.reserve(frames);
        backpressure.reserve(frames);
        receiver_front.reserve(frames);
        queue.reserve(frames);
        fec.reserve(frames);
        latency.reserve(frames);
        synchronization.reserve(frames);
        fft_csi.reserve(frames);
        fft_grid.reserve(frames);
        sfo_correction.reserve(frames);
        noise_estimation.reserve(frames);
        channel_estimation.reserve(frames);
        csi_smoothing.reserve(frames);
        detection.reserve(frames);
        control_header.reserve(frames);
        soft_demapping.reserve(frames);
        raw_noise_variance.reserve(frames);
        used_noise_variance.reserve(frames);
        std::ofstream csv(output);
        if (!csv) {
            throw std::runtime_error("cannot write " + output.string());
        }
        csv << "frame,serial_rx_wall_us,serial_receiver_front_us,serial_fec_us,"
               "pipeline_rx_submit_us,pipeline_submit_call_us,"
               "pipeline_backpressure_wait_us,pipeline_receiver_front_us,"
               "pipeline_synchronization_us,pipeline_fft_csi_us,"
               "pipeline_fft_grid_us,pipeline_sfo_correction_us,"
               "pipeline_noise_estimation_us,"
               "pipeline_channel_estimation_us,pipeline_csi_smoothing_us,"
               "pipeline_detection_adaptation_us,pipeline_control_header_us,"
               "pipeline_soft_demapping_us,"
               "pipeline_queue_wait_us,pipeline_fec_us,pipeline_latency_us,"
               "raw_noise_variance,used_noise_variance,"
               "buffer_slot,crc_ok,workspace_growths,ldpc_growths\n";
        csv << std::setprecision(10);
        for (std::size_t frame = 0u; frame < frames; ++frame) {
            const auto& item = results[frame];
            verify(item.link);
            if (item.frame_id != frame ||
                item.link.workspace_growths_this_frame != 0u ||
                item.link.ldpc_capacity_growths_this_frame != 0u) {
                throw std::runtime_error("warmed IQ pipeline order/growth failure");
            }
            producer.push_back(item.timing.producer_wall_us);
            submit_call.push_back(item.timing.submit_call_us);
            backpressure.push_back(item.timing.backpressure_wait_us);
            receiver_front.push_back(item.timing.receiver_front_us);
            queue.push_back(item.timing.queue_wait_us);
            fec.push_back(item.timing.fec_wall_us);
            latency.push_back(item.timing.latency_us);
            synchronization.push_back(item.link.timing.synchronization_us);
            fft_csi.push_back(item.link.timing.fft_csi_us);
            fft_grid.push_back(item.link.timing.fft_grid_us);
            sfo_correction.push_back(item.link.timing.sfo_correction_us);
            noise_estimation.push_back(item.link.timing.noise_estimation_us);
            channel_estimation.push_back(item.link.timing.channel_estimation_us);
            csi_smoothing.push_back(item.link.timing.csi_smoothing_us);
            detection.push_back(item.link.timing.detection_adaptation_us);
            control_header.push_back(item.link.timing.control_header_us);
            soft_demapping.push_back(item.link.timing.soft_demapping_us);
            raw_noise_variance.push_back(item.link.raw_noise_variance);
            used_noise_variance.push_back(item.link.noise_variance_used);
            csv << frame << ',' << serial_wall[frame] << ','
                << serial_front[frame] << ',' << serial_fec[frame] << ','
                << item.timing.producer_wall_us << ','
                << item.timing.submit_call_us << ','
                << item.timing.backpressure_wait_us << ','
                << item.timing.receiver_front_us << ','
                << item.link.timing.synchronization_us << ','
                << item.link.timing.fft_csi_us << ','
                << item.link.timing.fft_grid_us << ','
                << item.link.timing.sfo_correction_us << ','
                << item.link.timing.noise_estimation_us << ','
                << item.link.timing.channel_estimation_us << ','
                << item.link.timing.csi_smoothing_us << ','
                << item.link.timing.detection_adaptation_us << ','
                << item.link.timing.control_header_us << ','
                << item.link.timing.soft_demapping_us << ','
                << item.timing.queue_wait_us << ',' << item.timing.fec_wall_us
                << ',' << item.timing.latency_us << ','
                << item.link.raw_noise_variance << ','
                << item.link.noise_variance_used << ','
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
        const auto deadline_misses = count_over(submit_call, 225.0);
        summary << "frames,iq_corpus_frames,buffers,ldpc_workers,"
                   "scheduling_profile,scheduling_applied,"
                   "iq_generation_us_per_corpus_frame,"
                   "serial_rx_interval_us,pipeline_rx_interval_us,"
                   "pipeline_frames_per_second,rx_speedup,"
                   "rx_submit_mean_us,rx_submit_median_us,rx_submit_p99_us,"
                   "rx_submit_p999_us,rx_submit_max_us,"
                   "submit_call_mean_us,submit_call_median_us,"
                   "submit_call_p99_us,submit_call_p999_us,submit_call_max_us,"
                   "submit_deadline_misses,submit_deadline_miss_rate,"
                   "backpressure_mean_us,backpressure_p99_us,"
                   "backpressure_p999_us,backpressure_max_us,"
                   "receiver_front_mean_us,synchronization_mean_us,"
                   "fft_csi_mean_us,fft_grid_mean_us,sfo_correction_mean_us,"
                   "noise_estimation_mean_us,raw_noise_variance_mean,"
                   "used_noise_variance_mean,"
                   "channel_estimation_mean_us,csi_smoothing_mean_us,"
                   "detection_adaptation_mean_us,"
                   "control_header_mean_us,soft_demapping_mean_us,"
                   "fec_mean_us,fec_p99_us,fec_p999_us,fec_max_us,"
                   "queue_median_us,queue_p95_us,queue_p99_us,"
                   "queue_p999_us,queue_max_us,latency_median_us,"
                   "latency_p95_us,latency_p99_us,latency_p999_us,"
                   "latency_max_us,crc_success_rate,"
                   "post_warmup_growths\n";
        summary << std::setprecision(10) << frames << ',' << corpus_frames
                << ",2,8," << scheduling_profile << ',' << scheduling_applied
                << ',' << generation_total_us / corpus_frames << ','
                << serial_interval << ','
                << pipeline_interval << ',' << 1.0e6 / pipeline_interval << ','
                << serial_interval / pipeline_interval << ','
                << mean(producer) << ',' << percentile(producer, 0.50) << ','
                << percentile(producer, 0.99) << ','
                << percentile(producer, 0.999) << ',' << maximum(producer) << ','
                << mean(submit_call) << ',' << percentile(submit_call, 0.50) << ','
                << percentile(submit_call, 0.99) << ','
                << percentile(submit_call, 0.999) << ','
                << maximum(submit_call) << ',' << deadline_misses << ','
                << static_cast<double>(deadline_misses) / frames << ','
                << mean(backpressure) << ',' << percentile(backpressure, 0.99)
                << ',' << percentile(backpressure, 0.999) << ','
                << maximum(backpressure) << ',' << mean(receiver_front)
                << ',' << mean(synchronization) << ',' << mean(fft_csi) << ','
                << mean(fft_grid) << ',' << mean(sfo_correction) << ','
                << mean(noise_estimation) << ',' << mean(raw_noise_variance)
                << ',' << mean(used_noise_variance) << ','
                << mean(channel_estimation) << ',' << mean(csi_smoothing) << ','
                << mean(detection) << ',' << mean(control_header) << ','
                << mean(soft_demapping) << ',' << mean(fec) << ','
                << percentile(fec, 0.99) << ',' << percentile(fec, 0.999)
                << ',' << maximum(fec) << ','
                << percentile(queue, 0.50) << ','
                << percentile(queue, 0.95) << ',' << percentile(queue, 0.99)
                << ',' << percentile(queue, 0.999) << ',' << maximum(queue)
                << ',' << percentile(latency, 0.50) << ','
                << percentile(latency, 0.95) << ','
                << percentile(latency, 0.99) << ','
                << percentile(latency, 0.999) << ',' << maximum(latency)
                << ",1,0\n";

        std::cout << std::fixed << std::setprecision(2)
                  << "Frames: " << frames
                  << "; IQ corpus: " << corpus_frames
                  << "; two-channel IQ; 8 LDPC workers; scheduling "
                  << scheduling_profile << " (applied="
                  << scheduling_applied << ")\n"
                  << "Excluded IQ generation: "
                  << generation_total_us / corpus_frames
                  << " us/corpus frame\n"
                  << "Serial RX interval: " << serial_interval << " us/frame\n"
                  << "Pipeline RX interval: " << pipeline_interval
                  << " us/frame; " << 1.0e6 / pipeline_interval
                  << " frame/s; speedup "
                  << serial_interval / pipeline_interval << "x\n"
                  << "RX submit mean/median/p99: " << mean(producer) << '/'
                  << percentile(producer, 0.50) << '/'
                  << percentile(producer, 0.99) << " us\n"
                  << "Submit call median/p99/p99.9/max: "
                  << percentile(submit_call, 0.50) << '/'
                  << percentile(submit_call, 0.99) << '/'
                  << percentile(submit_call, 0.999) << '/'
                  << maximum(submit_call) << " us; >225 us "
                  << deadline_misses << '/' << frames << " ("
                  << 100.0 * static_cast<double>(deadline_misses) / frames
                  << "%)\n"
                  << "Backpressure mean/p99.9/max: " << mean(backpressure)
                  << '/' << percentile(backpressure, 0.999) << '/'
                  << maximum(backpressure) << " us\n"
                  << "RX stages mean sync/FFT-CSI/detect/control/demap: "
                  << mean(synchronization) << '/' << mean(fft_csi) << '/'
                  << mean(detection) << '/' << mean(control_header) << '/'
                  << mean(soft_demapping) << " us\n"
                  << "FFT/CSI mean FFT/SFO/noise/estimate/smooth: "
                  << mean(fft_grid) << '/' << mean(sfo_correction) << '/'
                  << mean(noise_estimation) << '/'
                  << mean(channel_estimation) << '/' << mean(csi_smoothing)
                  << " us\n"
                  << "Pilot noise raw/filtered mean: "
                  << std::scientific << std::setprecision(3)
                  << mean(raw_noise_variance) << '/'
                  << mean(used_noise_variance) << std::fixed
                  << std::setprecision(2) << '\n'
                  << "FEC mean/median/p99: " << mean(fec) << '/'
                  << percentile(fec, 0.50) << '/'
                  << percentile(fec, 0.99) << " us\n"
                  << "FEC p99.9/max: " << percentile(fec, 0.999) << '/'
                  << maximum(fec) << " us\n"
                  << "Queue median/p99: " << percentile(queue, 0.50) << '/'
                  << percentile(queue, 0.99) << " us\n"
                  << "RX latency median/p99: "
                  << percentile(latency, 0.50) << '/'
                  << percentile(latency, 0.99) << " us\n"
                  << "RX latency p99.9/max: "
                  << percentile(latency, 0.999) << '/'
                  << maximum(latency) << " us\n"
                  << "CRC success 100%; post-warm-up growths 0\n"
                  << "Wrote " << output.string() << " and "
                  << summary_path.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
