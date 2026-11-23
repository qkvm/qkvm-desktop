#pragma once

#include <array>
#include <cstdint>

// The HTTP status a harness's first byte says an answer came with (fuzz_redfish.h, fuzz_kvmd.h): the byte modulo
// sixteen, of the statuses a controller or a KVM answers with, 200 first. (The first builder's share of 600 never made
// 200, by which a resource is taken at all: the Redfish target's canary found it unreached, 2026-11-17.)
namespace qkvm::fuzz {

inline int statusOf(std::uint8_t said)
{
    constexpr std::array<int, 16> statuses{200, 400, 201, 202, 204, 401, 403, 404,
                                           405, 409, 412, 428, 500, 503, 302, 0};
    return statuses[said % statuses.size()];
}

} // namespace qkvm::fuzz
