#pragma once

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace openisac {

template <typename T>
std::vector<T> read_binary_vector(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("cannot open golden vector: " + path);
    }
    const std::streamsize byte_count = stream.tellg();
    if (byte_count < 0 || byte_count % static_cast<std::streamsize>(sizeof(T)) != 0) {
        throw std::runtime_error("golden vector has incompatible byte count: " + path);
    }
    stream.seekg(0, std::ios::beg);
    std::vector<T> values(static_cast<std::size_t>(byte_count) / sizeof(T));
    if (byte_count > 0 &&
        !stream.read(reinterpret_cast<char*>(values.data()), byte_count)) {
        throw std::runtime_error("cannot read golden vector: " + path);
    }
    return values;
}

inline std::string join_path(const std::string& directory, const std::string& file) {
#ifdef _WIN32
    return directory + "\\" + file;
#else
    return directory + "/" + file;
#endif
}

}  // namespace openisac
