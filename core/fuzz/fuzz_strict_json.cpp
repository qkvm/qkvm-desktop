#include "fuzz_strict_json.h"

#include <cstddef>
#include <cstdint>

// libFuzzer's way in (the fuzz preset: CMakeLists.txt, qkvm_add_fuzzer): the harness of fuzz_strict_json.h.
// (libFuzzer's name for it.)
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    qkvm::fuzz::readStrictJson({data, size});
    return 0;
}
