#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>

// What a fuzz test's program asks for, counted: every `new` of the program comes here, and while a `Watch` stands
// what is asked for on its thread is added up. Included by exactly one source of a program (it defines the
// program's own operator new and delete).
namespace qkvm::bmc::tests {

inline thread_local bool watching = false;
inline thread_local std::size_t asked = 0;
inline thread_local std::size_t largestAsked = 0;

// While it stands, what this thread asks for is counted, from nothing.
class Watch {
public:
    Watch()
    {
        asked = 0;
        largestAsked = 0;
        watching = true;
    }
    ~Watch() { watching = false; }
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
};

} // namespace qkvm::bmc::tests

// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory): the program's own allocator, counted
void* operator new(std::size_t size)
{
    if (qkvm::bmc::tests::watching) {
        qkvm::bmc::tests::asked += size;
        qkvm::bmc::tests::largestAsked = std::max(qkvm::bmc::tests::largestAsked, size);
    }
    if (void* found = std::malloc(size ? size : 1)) {
        return found;
    }
    throw std::bad_alloc();
}
void operator delete(void* gone) noexcept
{
    std::free(gone);
}
void operator delete(void* gone, std::size_t) noexcept
{
    std::free(gone);
}
// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
