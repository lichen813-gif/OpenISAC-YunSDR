#pragma once

#include "openisac/compute_backend.hpp"

#include <memory>

namespace openisac {

std::unique_ptr<PhyComputeBackend> make_cuda_compute_backend();

}  // namespace openisac
