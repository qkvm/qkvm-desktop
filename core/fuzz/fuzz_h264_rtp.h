#pragma once

#include <QByteArrayView>
#include <qkvm/base/fuzz_promise.h>
#include <qkvm/core/h264_rtp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// The harness of the H.264 a JetKVM and a kvmd KVM send over RTP, made into pictures (fuzz_h264_rtp.cpp, libFuzzer's
// way in; test_core_fuzz_found, what it once found): h264_rtp.cpp is compiled into the fuzz target. An input is one
// byte (its bit 0: the first two packets are the parameter sets a description carried, SPS and PPS), then packets,
// each its length in two bytes (big-endian; its top bit: a packet is lost before it) and its bytes. Held to the
// reader's promises: no picture larger than one assembly and its parameter sets, and none of nothing.
//
// Its canary (fuzz_promise.h): after a packet was lost, a key picture assembled from two or more packets of one time
// (no seed loses one).
namespace qkvm::fuzz {

inline void readH264Rtp(std::span<const std::uint8_t> input)
{
    if (input.empty()) {
        return;
    }
    core::H264Rtp reader;
    std::span<const std::uint8_t> rest = input.subspan(1);
    const auto next = [&rest](bool& lost) -> std::optional<QByteArrayView> {
        if (rest.size() < 2) {
            return std::nullopt;
        }
        const std::size_t said = (std::size_t{rest[0]} << 8) | rest[1];
        lost = (said & 0x8000) != 0;
        const std::size_t length = std::min(said & 0x7FFF, rest.size() - 2);
        const QByteArrayView packet(reinterpret_cast<const char*>(rest.data()) + 2, static_cast<qsizetype>(length));
        rest = rest.subspan(2 + length);
        return packet;
    };
    bool lost = false;
    if ((input[0] & 1) != 0) {
        const std::optional<QByteArrayView> sps = next(lost);
        const std::optional<QByteArrayView> pps = next(lost);
        if (sps && pps) {
            static_cast<void>(reader.parameters(*sps, *pps));
        }
    }
    int packets = 0;
    bool lostOnce = false;
    // (Packets of the one time running, by their RTP headers' own, read here: the reader says no more than a picture.)
    std::optional<QByteArrayView> time;
    int ofTheTime = 0;
    while (const std::optional<QByteArrayView> packet = next(lost)) {
        if (++packets > 512) {
            break;
        }
        if (lost) {
            reader.lose();
            lostOnce = true;
        }
        const std::optional<QByteArrayView> its =
            packet->size() >= 12 ? std::optional<QByteArrayView>(packet->sliced(4, 4)) : std::nullopt;
        ofTheTime = its && time && *its == *time ? ofTheTime + 1 : 1;
        time = its;
        if (const std::optional<core::H264Rtp::Picture> picture = reader.take(*packet)) {
            promise(picture->bytes.size() <= core::H264Rtp::largestPicture + qsizetype{2} * 65536 + 64,
                    "a picture larger than one assembly");
            promise(!picture->bytes.isEmpty(), "a picture of nothing");
            if (lostOnce && picture->key && ofTheTime >= 2) {
                reached("after a packet was lost, a key picture assembled from two or more packets of one time");
            }
        }
    }
}

} // namespace qkvm::fuzz
