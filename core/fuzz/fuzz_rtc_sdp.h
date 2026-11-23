#pragma once

#include <QByteArray>
#include <QList>
#include <qkvm/base/fuzz_promise.h>

#include <cstdint>
#include <optional>
#include <span>

#include "../src/rtc_sdp.h"

// The harness of what a far end says of its session and of its addresses, a JetKVM's answer and a kvmd KVM's Janus
// alike (fuzz_rtc_sdp.cpp, libFuzzer's way in; test_core_fuzz_found, what it once found): rtc_sdp.cpp is compiled into
// the fuzz target. An input is one byte (bit 0: this PC's loopback is allowed, as for a test's stand-in; bit 1: only
// the payloads an offer of this program's names are taken, else any), then a session description. Its video is chosen
// as the receiving side chooses it, its application looked for, and its addresses taken out of it and each judged.
// Held to the readers' promises: a video of a payload there is and a mid that is a short token, and an address that is
// judged sound is one that is judged the same again, made of the fields judged.
//
// Its canary (fuzz_promise.h): a description read as an answer to an offer of this program's (only its payloads taken),
// whose video was chosen with the parameter sets it carried, beside an address of it judged sound (the seeds that carry
// parameter sets are read as Janus's offers).
namespace qkvm::fuzz {

inline void readRtcSdp(std::span<const std::uint8_t> input)
{
    if (input.empty()) {
        return;
    }
    const bool loopback = (input[0] & 1) != 0;
    const QList<int> only = (input[0] & 2) != 0 ? QList<int>{96, 102} : QList<int>{};
    const QByteArray sdp(reinterpret_cast<const char*>(input.data()) + 1, static_cast<qsizetype>(input.size() - 1));
    bool carried = false;
    if (const std::optional<core::VideoChoice> video = core::chooseVideo(sdp, only)) {
        carried = !only.isEmpty() && !video->sps.isEmpty() && !video->pps.isEmpty();
        promise(video->payload >= 0 && video->payload <= 127, "a video of a payload there is none of");
        promise(only.isEmpty() || only.contains(video->payload), "a video of a payload that was not offered");
        promise(!video->mid.isEmpty() && video->mid.size() <= 64, "a video's mid that is no short token");
        promise(video->sps.size() <= 65536 && video->pps.size() <= 65536, "parameters longer than a description");
    }
    static_cast<void>(core::hasApplication(sdp));
    if (const std::optional<QByteArray> mid = core::applicationMid(sdp)) {
        promise(mid->size() <= sdp.size(), "an application's mid longer than its description");
    }
    const core::WithoutAddresses without = core::withoutAddresses(sdp);
    // (Its last line ended, where it was not.)
    promise(without.rest.size() <= sdp.size() + 1, "a description longer without its addresses");
    int judged = 0;
    for (const QByteArray& candidate : without.candidates) {
        if (++judged > 64) {
            break;
        }
        const core::JudgedAddress first = core::judgeAddress(candidate, loopback);
        if (first.fault != core::AddressFault::None) {
            continue;
        }
        promise(first.candidate.startsWith("candidate:"), "a sound address that is no candidate");
        const core::JudgedAddress again = core::judgeAddress(first.candidate, loopback);
        promise(again.fault == core::AddressFault::None && again.candidate == first.candidate,
                "a sound address judged otherwise when it is judged again");
        if (carried) {
            reached("an answer whose video was chosen with the parameter sets it carried, beside a sound address");
        }
    }
}

} // namespace qkvm::fuzz
