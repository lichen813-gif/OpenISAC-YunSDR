#include "openisac/capture_io.hpp"

#include "openisac/dynamic_link_pipeline.hpp"

#include <algorithm>
#include <array>
#include <complex>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace openisac {
namespace {

constexpr std::array<char, 8> file_magic{
    'O', 'I', 'S', 'A', 'C', 'I', 'Q', '1'};
constexpr std::uint32_t endian_marker = 0x01020304u;
constexpr std::uint32_t file_version = 1u;
constexpr std::uint32_t complex_float32_format = 1u;
constexpr std::uint32_t frame_marker = 0x314D5246u;
constexpr std::uint32_t frame_header_bytes = 40u;
constexpr std::size_t bytes_per_complex_float = 8u;
constexpr std::size_t maximum_file_antennas = 64u;

void write_exact(std::ostream& stream, const char* data, std::size_t size) {
    stream.write(data, static_cast<std::streamsize>(size));
    if (!stream) {
        throw std::runtime_error("cannot write IQ capture file");
    }
}

void write_u32(std::ostream& stream, std::uint32_t value) {
    std::array<char, 4> bytes{};
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (8u * index)) & 0xFFu);
    }
    write_exact(stream, bytes.data(), bytes.size());
}

void write_u64(std::ostream& stream, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (8u * index)) & 0xFFu);
    }
    write_exact(stream, bytes.data(), bytes.size());
}

void encode_float(char* destination, float value) {
    std::uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(value), "float32 is required");
    std::memcpy(&bits, &value, sizeof(bits));
    for (std::size_t index = 0u; index < 4u; ++index) {
        destination[index] = static_cast<char>(
            (bits >> (8u * index)) & 0xFFu);
    }
}

void read_exact(std::istream& stream, char* data, std::size_t size) {
    stream.read(data, static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error("truncated IQ capture file");
    }
}

std::uint32_t decode_u32(const std::array<char, 4>& bytes) {
    std::uint32_t value = 0u;
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(bytes[index])) << (8u * index);
    }
    return value;
}

std::uint32_t read_u32(std::istream& stream) {
    std::array<char, 4> bytes{};
    read_exact(stream, bytes.data(), bytes.size());
    return decode_u32(bytes);
}

bool try_read_u32(std::istream& stream, std::uint32_t& value) {
    std::array<char, 4> bytes{};
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = stream.gcount();
    if (count == 0 && stream.eof()) {
        return false;
    }
    if (count != static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error("truncated IQ capture frame marker");
    }
    value = decode_u32(bytes);
    return true;
}

std::uint64_t read_u64(std::istream& stream) {
    std::array<char, 8> bytes{};
    read_exact(stream, bytes.data(), bytes.size());
    std::uint64_t value = 0u;
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(
            static_cast<unsigned char>(bytes[index])) << (8u * index);
    }
    return value;
}

float decode_float(const char* source) {
    std::uint32_t bits = 0u;
    for (std::size_t index = 0u; index < 4u; ++index) {
        bits |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(source[index])) << (8u * index);
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void validate_frame_shape(
    const DynamicLinkCaptureFrame& frame,
    std::size_t antenna_count,
    std::size_t maximum_samples_per_antenna) {
    if (frame.samples.size() != antenna_count || antenna_count == 0u ||
        frame.samples.front().empty()) {
        throw std::invalid_argument("capture frame has an invalid antenna layout");
    }
    const std::size_t samples = frame.samples.front().size();
    if (samples > maximum_samples_per_antenna) {
        throw std::invalid_argument("capture frame exceeds configured sample capacity");
    }
    for (const auto& branch : frame.samples) {
        if (branch.size() != samples) {
            throw std::invalid_argument("capture frame branches have different lengths");
        }
    }
}

void track_sequence(
    std::uint64_t sequence,
    bool& valid,
    std::uint64_t& last,
    std::uint64_t& gaps,
    std::uint64_t* out_of_order) {
    if (!valid) {
        valid = true;
        last = sequence;
        return;
    }
    if (sequence > last) {
        if (last != std::numeric_limits<std::uint64_t>::max() &&
            sequence > last + 1u) {
            gaps += sequence - last - 1u;
        }
        last = sequence;
    } else if (out_of_order != nullptr) {
        ++(*out_of_order);
    }
}

}  // namespace

struct CaptureRingBuffer::Impl {
    Impl(
        std::size_t frame_capacity,
        std::size_t antennas,
        std::size_t sample_capacity)
        : capacity_frames(frame_capacity), antenna_count_value(antennas),
          maximum_samples(sample_capacity), slots(frame_capacity) {
        if (frame_capacity == 0u || antennas == 0u || sample_capacity == 0u) {
            throw std::invalid_argument("capture ring dimensions must be non-zero");
        }
        for (auto& slot : slots) {
            slot.samples.resize(antennas);
            for (auto& branch : slot.samples) {
                branch.resize(sample_capacity);
            }
        }
    }

    const std::size_t capacity_frames;
    const std::size_t antenna_count_value;
    const std::size_t maximum_samples;
    std::vector<DynamicLinkCaptureFrame> slots;
    std::size_t read_index = 0u;
    std::size_t write_index = 0u;
    std::size_t count = 0u;
    CaptureRingStatistics stats{};
    bool source_sequence_valid = false;
    std::uint64_t last_source_sequence = 0u;
    bool consumer_sequence_valid = false;
    std::uint64_t last_consumer_sequence = 0u;
    bool timestamp_valid = false;
    std::uint64_t last_timestamp = 0u;
    mutable std::mutex mutex;
};

CaptureRingBuffer::CaptureRingBuffer(
    std::size_t capacity_frames,
    std::size_t antenna_count,
    std::size_t maximum_samples_per_antenna)
    : impl_(new Impl(
          capacity_frames, antenna_count, maximum_samples_per_antenna)) {}

CaptureRingBuffer::~CaptureRingBuffer() = default;

bool CaptureRingBuffer::try_push(const DynamicLinkCaptureFrame& frame) {
    validate_frame_shape(
        frame, impl_->antenna_count_value, impl_->maximum_samples);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->stats.push_attempts;
    track_sequence(
        frame.capture_sequence,
        impl_->source_sequence_valid,
        impl_->last_source_sequence,
        impl_->stats.source_sequence_gaps,
        &impl_->stats.out_of_order_frames);
    if (impl_->timestamp_valid && frame.timestamp < impl_->last_timestamp) {
        ++impl_->stats.timestamp_regressions;
    }
    if (!impl_->timestamp_valid || frame.timestamp > impl_->last_timestamp) {
        impl_->last_timestamp = frame.timestamp;
    }
    impl_->timestamp_valid = true;
    if (impl_->count == impl_->capacity_frames) {
        ++impl_->stats.overflow_drops;
        return false;
    }

    auto& slot = impl_->slots[impl_->write_index];
    slot.capture_sequence = frame.capture_sequence;
    slot.timestamp = frame.timestamp;
    slot.pilot_seed = frame.pilot_seed;
    const std::size_t samples = frame.samples.front().size();
    for (std::size_t antenna = 0u;
         antenna < impl_->antenna_count_value; ++antenna) {
        slot.samples[antenna].resize(samples);
        std::copy(
            frame.samples[antenna].begin(), frame.samples[antenna].end(),
            slot.samples[antenna].begin());
    }
    impl_->write_index = (impl_->write_index + 1u) % impl_->capacity_frames;
    ++impl_->count;
    ++impl_->stats.frames_pushed;
    impl_->stats.high_watermark = std::max(
        impl_->stats.high_watermark, impl_->count);
    return true;
}

bool CaptureRingBuffer::try_pop(DynamicLinkCaptureFrame& frame) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->count == 0u) {
        return false;
    }
    const auto& slot = impl_->slots[impl_->read_index];
    frame.capture_sequence = slot.capture_sequence;
    frame.timestamp = slot.timestamp;
    frame.pilot_seed = slot.pilot_seed;
    frame.samples.resize(impl_->antenna_count_value);
    for (std::size_t antenna = 0u;
         antenna < impl_->antenna_count_value; ++antenna) {
        frame.samples[antenna].resize(slot.samples[antenna].size());
        std::copy(
            slot.samples[antenna].begin(), slot.samples[antenna].end(),
            frame.samples[antenna].begin());
    }
    track_sequence(
        frame.capture_sequence,
        impl_->consumer_sequence_valid,
        impl_->last_consumer_sequence,
        impl_->stats.consumer_sequence_gaps,
        nullptr);
    impl_->read_index = (impl_->read_index + 1u) % impl_->capacity_frames;
    --impl_->count;
    ++impl_->stats.frames_popped;
    return true;
}

std::size_t CaptureRingBuffer::size() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->count;
}

std::size_t CaptureRingBuffer::capacity() const noexcept {
    return impl_->capacity_frames;
}

std::size_t CaptureRingBuffer::antenna_count() const noexcept {
    return impl_->antenna_count_value;
}

std::size_t CaptureRingBuffer::maximum_samples_per_antenna() const noexcept {
    return impl_->maximum_samples;
}

CaptureRingStatistics CaptureRingBuffer::statistics() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

struct IqCaptureFileWriter::Impl {
    Impl(const std::string& path, std::size_t antennas)
        : antenna_count_value(antennas), stream(path, std::ios::binary) {
        if (antennas == 0u || antennas > maximum_file_antennas) {
            throw std::invalid_argument("invalid IQ file antenna count");
        }
        if (!stream) {
            throw std::runtime_error("cannot create IQ capture file: " + path);
        }
        write_exact(stream, file_magic.data(), file_magic.size());
        write_u32(stream, endian_marker);
        write_u32(stream, file_version);
        write_u32(stream, static_cast<std::uint32_t>(antennas));
        write_u32(stream, complex_float32_format);
        write_u64(stream, 0u);
    }

    const std::size_t antenna_count_value;
    std::ofstream stream;
    std::vector<char> payload_buffer;
    std::uint64_t frame_count = 0u;
};

IqCaptureFileWriter::IqCaptureFileWriter(
    const std::string& path,
    std::size_t antenna_count)
    : impl_(new Impl(path, antenna_count)) {}

IqCaptureFileWriter::~IqCaptureFileWriter() = default;

void IqCaptureFileWriter::append(const DynamicLinkCaptureFrame& frame) {
    validate_frame_shape(
        frame, impl_->antenna_count_value,
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
    const std::size_t samples = frame.samples.front().size();
    if (impl_->antenna_count_value >
        std::numeric_limits<std::uint64_t>::max() /
            samples / bytes_per_complex_float) {
        throw std::overflow_error("IQ capture payload size overflow");
    }
    const auto payload_bytes = static_cast<std::uint64_t>(
        impl_->antenna_count_value * samples * bytes_per_complex_float);
    write_u32(impl_->stream, frame_marker);
    write_u32(impl_->stream, frame_header_bytes);
    write_u64(impl_->stream, frame.capture_sequence);
    write_u64(impl_->stream, frame.timestamp);
    write_u32(impl_->stream, frame.pilot_seed);
    write_u32(impl_->stream, static_cast<std::uint32_t>(samples));
    write_u64(impl_->stream, payload_bytes);
    impl_->payload_buffer.resize(static_cast<std::size_t>(payload_bytes));
    std::size_t offset = 0u;
    for (const auto& branch : frame.samples) {
        for (const auto sample : branch) {
            encode_float(impl_->payload_buffer.data() + offset, sample.real());
            encode_float(
                impl_->payload_buffer.data() + offset + 4u, sample.imag());
            offset += bytes_per_complex_float;
        }
    }
    write_exact(
        impl_->stream, impl_->payload_buffer.data(),
        impl_->payload_buffer.size());
    ++impl_->frame_count;
}

void IqCaptureFileWriter::flush() {
    impl_->stream.flush();
    if (!impl_->stream) {
        throw std::runtime_error("cannot flush IQ capture file");
    }
}

std::uint64_t IqCaptureFileWriter::frames_written() const noexcept {
    return impl_->frame_count;
}

struct IqCaptureFileReader::Impl {
    Impl(const std::string& path, std::size_t maximum_samples)
        : maximum_samples_per_antenna(maximum_samples),
          stream(path, std::ios::binary) {
        if (maximum_samples == 0u) {
            throw std::invalid_argument("IQ reader sample limit must be non-zero");
        }
        if (!stream) {
            throw std::runtime_error("cannot open IQ capture file: " + path);
        }
        std::array<char, 8> magic{};
        read_exact(stream, magic.data(), magic.size());
        if (magic != file_magic || read_u32(stream) != endian_marker ||
            read_u32(stream) != file_version) {
            throw std::runtime_error("unsupported IQ capture file header");
        }
        antenna_count_value = read_u32(stream);
        if (antenna_count_value == 0u ||
            antenna_count_value > maximum_file_antennas ||
            read_u32(stream) != complex_float32_format) {
            throw std::runtime_error("unsupported IQ capture sample format");
        }
        (void)read_u64(stream);
    }

    const std::size_t maximum_samples_per_antenna;
    std::ifstream stream;
    std::size_t antenna_count_value = 0u;
    std::vector<char> payload_buffer;
    std::uint64_t frame_count = 0u;
};

IqCaptureFileReader::IqCaptureFileReader(
    const std::string& path,
    std::size_t maximum_samples_per_antenna)
    : impl_(new Impl(path, maximum_samples_per_antenna)) {}

IqCaptureFileReader::~IqCaptureFileReader() = default;

bool IqCaptureFileReader::read_next(DynamicLinkCaptureFrame& frame) {
    std::uint32_t marker = 0u;
    if (!try_read_u32(impl_->stream, marker)) {
        return false;
    }
    if (marker != frame_marker || read_u32(impl_->stream) != frame_header_bytes) {
        throw std::runtime_error("invalid IQ capture frame header");
    }
    frame.capture_sequence = read_u64(impl_->stream);
    frame.timestamp = read_u64(impl_->stream);
    frame.pilot_seed = read_u32(impl_->stream);
    const std::size_t samples = read_u32(impl_->stream);
    const std::uint64_t payload_bytes = read_u64(impl_->stream);
    if (samples == 0u || samples > impl_->maximum_samples_per_antenna ||
        impl_->antenna_count_value >
            std::numeric_limits<std::uint64_t>::max() /
                samples / bytes_per_complex_float ||
        payload_bytes != static_cast<std::uint64_t>(
            impl_->antenna_count_value * samples * bytes_per_complex_float)) {
        throw std::runtime_error("invalid IQ capture frame dimensions");
    }
    if (payload_bytes > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("IQ capture frame is too large for this build");
    }
    impl_->payload_buffer.resize(static_cast<std::size_t>(payload_bytes));
    read_exact(
        impl_->stream, impl_->payload_buffer.data(),
        impl_->payload_buffer.size());
    frame.samples.resize(impl_->antenna_count_value);
    std::size_t offset = 0u;
    for (auto& branch : frame.samples) {
        branch.resize(samples);
        for (auto& sample : branch) {
            const float real = decode_float(
                impl_->payload_buffer.data() + offset);
            const float imag = decode_float(
                impl_->payload_buffer.data() + offset + 4u);
            sample = {real, imag};
            offset += bytes_per_complex_float;
        }
    }
    ++impl_->frame_count;
    return true;
}

std::size_t IqCaptureFileReader::antenna_count() const noexcept {
    return impl_->antenna_count_value;
}

std::uint64_t IqCaptureFileReader::frames_read() const noexcept {
    return impl_->frame_count;
}

bool submit_next_capture(
    CaptureRingBuffer& ring,
    DynamicLinkPipeline& pipeline,
    const DynamicLinkReceiverConfig& receiver_config,
    DynamicLinkCaptureFrame& scratch) {
    if (!ring.try_pop(scratch)) {
        return false;
    }
    pipeline.submit_capture(
        scratch.capture_sequence, scratch, receiver_config);
    return true;
}

}  // namespace openisac
