#include "libyunsdr_isac/backend.hpp"

#include <stdexcept>

int main() {
    if (libyunsdr_isac::hardware_backend_available()) {
        return 1;
    }

    try {
        (void)libyunsdr_isac::make_device({});
    } catch (const std::runtime_error&) {
        return 0;
    }

    return 2;
}
