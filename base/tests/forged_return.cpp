#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

#include <intrin.h>

// The child of test_guarded_returns, built twice (base/CMakeLists.txt): as every program of ours is
// (qkvm-test-forged-return: qkvm_build, ARM64's /guard:signret and a release's /CETCOMPAT), and without that guard of
// returns (qkvm-test-forged-return-unguarded). Linked with what every test program is: it fails quietly.
//   guards   says what Windows gives this process: "pointer-auth N shadow-stack N" (1 on, 0 off or not known here)
//   plain    returns from forge() as any function does: says RETURNED, ends 0
//   forge    overwrites forge()'s own return address on its stack with landed()'s: where nothing guards the return,
//            forge() "returns" into landed(), which ends the program with 42 at once; a guarded return must not get
//            there (Windows ends the program with a fast fail, 0xC0000409)
namespace {

constexpr UINT landedThere = 42;

// Reached only by a return to the overwritten address: it ends the program and does nothing else (entered by a return
// rather than a call, its stack is not as a function's should be, so it calls nothing that could mind).
[[noreturn]] __declspec(noinline) void landed()
{
    TerminateProcess(GetCurrentProcess(), landedThere);
    std::abort(); // (TerminateProcess does not return to its own process)
}

// Where the return goes when it is overwritten, read through a volatile so that the compiler keeps the store.
void* volatile const landing = reinterpret_cast<void*>(&landed);

__declspec(noinline) int forge(bool overwrite)
{
    volatile int kept = overwrite ? 1 : 0;
    if (overwrite) {
        // The place on this function's stack where its return address is kept (saved there as it began: on ARM64
        // signed first, where the guard is on).
        auto* returnTo = static_cast<void* volatile*>(_AddressOfReturnAddress());
        *returnTo = landing;
    }
    return kept + 1;
}

int guards()
{
    PROCESS_MITIGATION_USER_POINTER_AUTH_POLICY pointerAuth{};
    const bool pointerAuthRead = GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserPointerAuthPolicy,
                                                            &pointerAuth, sizeof pointerAuth) != FALSE;
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY shadowStack{};
    const bool shadowStackRead = GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy,
                                                            &shadowStack, sizeof shadowStack) != FALSE;
    std::printf("pointer-auth %d shadow-stack %d\n", pointerAuthRead && pointerAuth.EnablePointerAuthUserIp ? 1 : 0,
                shadowStackRead && shadowStack.EnableUserShadowStack ? 1 : 0);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fputs("forged_return: guards, plain or forge\n", stderr);
        return 2;
    }
    if (std::strcmp(argv[1], "guards") == 0) {
        return guards();
    }
    if (std::strcmp(argv[1], "plain") != 0 && std::strcmp(argv[1], "forge") != 0) {
        std::fputs("forged_return: guards, plain or forge\n", stderr);
        return 2;
    }
    const int result = forge(std::strcmp(argv[1], "forge") == 0);
    std::printf("RETURNED %d\n", result);
    return 0;
}
