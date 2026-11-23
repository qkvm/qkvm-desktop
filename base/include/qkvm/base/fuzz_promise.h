#pragma once

#include <cstdio>
#include <cstdlib>

// What every fuzz target's harness says where a reader broke one of its promises (DESIGN.md section 8): the promise,
// on stderr, and then abort(), which libFuzzer hears and keeps the input of; in test_core_fuzz_found, which runs the
// same harnesses on what they once found, the test program ends there with the same words (its quiet failures).
namespace qkvm::fuzz {

[[noreturn]] inline void broken(const char* promise)
{
    std::fprintf(stderr, "a promise broken: %s\n", promise);
    std::abort();
}

// Where a promise is held, the harness says so.
inline void promise(bool held, const char* promise)
{
    if (!held) {
        broken(promise);
    }
}

// Each harness's canary: where the reader has read an input through to a deep state of its own and said it was well,
// the harness names that state here (one state a harness, which none of its seeds reaches). In the canary build
// (QKVM_FUZZ_CANARY, the fuzz-canary preset: tools/fuzz.ps1 -Canary) that ends the run as a finding would, which shows
// that the fuzz target drives the reader that deep within its bounded run, from its seeds or from one small seed: a
// harness that never got past the reader's first checks, or a reader built without libFuzzer's eyes on it, would not
// find it. In every other build it is told to whoever listens (test_core_fuzz_found, which holds every input that once
// found a canary to finding it still), and is nothing else.
using Listener = void (*)(const char* state);

inline Listener& listener()
{
    static Listener heard = nullptr;
    return heard;
}

inline void reached(const char* state)
{
#if defined(QKVM_FUZZ_CANARY)
    std::fprintf(stderr, "the canary found: %s\n", state);
    std::abort();
#else
    if (const Listener heard = listener(); heard != nullptr) {
        heard(state);
    }
#endif
}

} // namespace qkvm::fuzz
