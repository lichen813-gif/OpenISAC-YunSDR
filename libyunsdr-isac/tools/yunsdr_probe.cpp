#include "libyunsdr_isac/backend.hpp"

#include <iostream>

int main() {
    std::cout << "libyunsdr-isac hardware probe\n";
    if (!libyunsdr_isac::hardware_backend_available()) {
        std::cout
            << "status: vendor backend is not installed\n"
            << "next: import the verified libyunsdr SDK and handoff package\n";
        return 0;
    }

    std::cout << "status: vendor backend is available\n";
    return 0;
}
