#pragma once

#include <chrono>
#include <cstdint>

namespace qkvm::base {

// Now, in microseconds, on the clock that only goes forward. Every interval in the program is
// measured on it; the clock on the wall is for log lines alone (DESIGN.md section 10).
inline std::int64_t steadyNowUs() noexcept
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace qkvm::base
