// Original QKVM trial code. It uses published libdatachannel and FFmpeg APIs; no upstream
// example code is incorporated. This is not a production driver or device signaling adapter.
#include <rtc/rtc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t maxUnitBytes = 8U * 1024U * 1024U;
constexpr std::size_t maxFixtureBytes = 64U * 1024U * 1024U;
constexpr std::uint8_t payloadType = 96;
constexpr std::uint32_t videoSsrc = 0x514b564d;
constexpr std::uint32_t firstTimestamp = 90000;
constexpr std::uint32_t timestampStep = 3000;
constexpr auto operationTimeout = 10s;

void require(bool condition, const std::string& problem)
{
    if (!condition) {
        throw std::runtime_error(problem);
    }
}

void checked(int code, const char* operation)
{
    if (code < 0) {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
        av_strerror(code, message.data(), message.size());
        throw std::runtime_error(std::string(operation) + ": " + message.data());
    }
}

struct FreeContext {
    void operator()(AVCodecContext* value) const { avcodec_free_context(&value); }
};
struct FreeParser {
    void operator()(AVCodecParserContext* value) const { av_parser_close(value); }
};
struct FreeFrame {
    void operator()(AVFrame* value) const { av_frame_free(&value); }
};
struct FreePacket {
    void operator()(AVPacket* value) const { av_packet_free(&value); }
};
using Context = std::unique_ptr<AVCodecContext, FreeContext>;
using Parser = std::unique_ptr<AVCodecParserContext, FreeParser>;
using Frame = std::unique_ptr<AVFrame, FreeFrame>;
using Packet = std::unique_ptr<AVPacket, FreePacket>;

// Compare NAL payloads exactly while allowing the standardized three/four-byte Annex B
// separators to differ. This routine neither implements RTP nor decodes H.264.
Bytes canonicalAnnexB(std::span<const std::uint8_t> bytes)
{
    struct Boundary {
        std::size_t begin;
        std::size_t payload;
    };
    std::vector<Boundary> boundaries;
    for (std::size_t at = 0; at + 2 < bytes.size();) {
        if (bytes[at] == 0 && bytes[at + 1] == 0) {
            const std::size_t size =
                bytes[at + 2] == 1 ? 3U : (at + 3 < bytes.size() && bytes[at + 2] == 0 && bytes[at + 3] == 1 ? 4U : 0U);
            if (size != 0) {
                boundaries.push_back({at, at + size});
                at += size;
                continue;
            }
        }
        ++at;
    }
    require(!boundaries.empty() && boundaries.front().begin == 0, "Expected Annex B access unit");
    Bytes result;
    for (std::size_t index = 0; index < boundaries.size(); ++index) {
        const std::size_t begin = boundaries[index].payload;
        const std::size_t end = index + 1 < boundaries.size() ? boundaries[index + 1].begin : bytes.size();
        require(begin < end, "Empty H.264 NAL unit in fixture or received frame");
        result.insert(result.end(), {0, 0, 0, 1});
        result.insert(result.end(), bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                      bytes.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return result;
}

std::size_t largestNal(const std::vector<Bytes>& units)
{
    std::size_t largest = 0;
    for (const auto& unit : units) {
        std::size_t payload = 4;
        for (std::size_t at = 4; at + 3 < unit.size(); ++at) {
            if (unit[at] == 0 && unit[at + 1] == 0 && unit[at + 2] == 0 && unit[at + 3] == 1) {
                largest = std::max(largest, at - payload);
                payload = at + 4;
                at += 3;
            }
        }
        largest = std::max(largest, unit.size() - payload);
    }
    return largest;
}

std::vector<Bytes> loadUnits(const std::filesystem::path& path, std::size_t wanted)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input.good(), "Cannot read fixture");
    const auto length = input.tellg();
    require(length > 0 && length <= static_cast<std::streamoff>(maxFixtureBytes), "Fixture must be 1..64 MiB");
    const auto size = static_cast<std::size_t>(length);
    Bytes bytes(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    require(input.good(), "Fixture read was incomplete");
    Parser parser(av_parser_init(AV_CODEC_ID_H264));
    Context context(avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264)));
    require(parser != nullptr && context != nullptr, "FFmpeg parser allocation failed");
    std::vector<Bytes> result;
    std::size_t cursor = 0;
    while (cursor < size && result.size() < wanted) {
        std::uint8_t* packet = nullptr;
        int packetSize = 0;
        const int consumed = av_parser_parse2(parser.get(), context.get(), &packet, &packetSize, bytes.data() + cursor,
                                              static_cast<int>(size - cursor), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        checked(consumed, "parse fixture");
        require(consumed > 0 || packetSize > 0, "Fixture parser made no progress");
        cursor += static_cast<std::size_t>(consumed);
        if (packetSize > 0) {
            require(static_cast<std::size_t>(packetSize) <= maxUnitBytes, "Access unit exceeds 8 MiB");
            result.push_back(canonicalAnnexB({packet, static_cast<std::size_t>(packetSize)}));
        }
    }
    if (result.size() < wanted) {
        std::uint8_t* packet = nullptr;
        int packetSize = 0;
        checked(av_parser_parse2(parser.get(), context.get(), &packet, &packetSize, nullptr, 0, AV_NOPTS_VALUE,
                                 AV_NOPTS_VALUE, 0),
                "flush fixture parser");
        if (packetSize > 0) {
            require(static_cast<std::size_t>(packetSize) <= maxUnitBytes, "Access unit exceeds 8 MiB");
            result.push_back(canonicalAnnexB({packet, static_cast<std::size_t>(packetSize)}));
        }
    }
    require(result.size() == wanted, "Fixture contains fewer access units than requested");
    return result;
}

struct Decoded {
    int width = 0;
    int height = 0;
    int format = 0;
    std::uint64_t hash = 0;
    bool operator==(const Decoded&) const = default;
};

// Use the same existing FFmpeg decoder libraries as the desktop. This checks software decode
// and active image bytes; it does not instantiate or benchmark the production D3D11 renderer.
std::vector<Decoded> decode(const std::vector<Bytes>& units)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    require(codec != nullptr, "Existing FFmpeg build has no H.264 decoder");
    Context context(avcodec_alloc_context3(codec));
    Frame frame(av_frame_alloc());
    Packet packet(av_packet_alloc());
    require(context != nullptr && frame != nullptr && packet != nullptr, "FFmpeg allocation failed");
    context->thread_count = 1;
    checked(avcodec_open2(context.get(), codec, nullptr), "open H.264 decoder");
    std::vector<Decoded> result;
    const auto drain = [&] {
        for (;;) {
            const int status = avcodec_receive_frame(context.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
                return;
            }
            checked(status, "decode H.264 access unit");
            require(frame->width > 0 && frame->width <= 4096 && frame->height > 0 && frame->height <= 2304,
                    "Decoded image outside desktop dimension bounds");
            const auto format = static_cast<AVPixelFormat>(frame->format);
            const int size = av_image_get_buffer_size(format, frame->width, frame->height, 1);
            checked(size, "calculate decoded image size");
            require(size <= 64 * 1024 * 1024, "Decoded image exceeds trial limit");
            Bytes pixels(static_cast<std::size_t>(size));
            checked(av_image_copy_to_buffer(pixels.data(), size, frame->data, frame->linesize, format, frame->width,
                                            frame->height, 1),
                    "copy active decoded image bytes");
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto pixel : pixels) {
                hash = (hash ^ pixel) * 1099511628211ULL;
            }
            result.push_back({frame->width, frame->height, frame->format, hash});
            av_frame_unref(frame.get());
        }
    };
    for (const auto& unit : units) {
        checked(av_new_packet(packet.get(), static_cast<int>(unit.size())), "allocate decoder packet");
        std::memcpy(packet->data, unit.data(), unit.size());
        checked(avcodec_send_packet(context.get(), packet.get()), "submit H.264 packet");
        av_packet_unref(packet.get());
        drain();
    }
    checked(avcodec_send_packet(context.get(), nullptr), "flush H.264 decoder");
    drain();
    require(result.size() == units.size(), "Decoded frame count differs from access-unit count");
    return result;
}

void requireLoopback(rtc::Candidate candidate)
{
    require(candidate.resolve(rtc::Candidate::ResolveMode::Simple), "Candidate is not a numeric IP address");
    require(candidate.address() == std::optional<std::string>("127.0.0.1"), "Refusing non-loopback ICE candidate");
    require(candidate.transportType() == rtc::Candidate::TransportType::Udp, "Refusing non-UDP candidate");
    require(candidate.type() == rtc::Candidate::Type::Host, "Refusing non-host candidate");
}

rtc::Description localDescription(rtc::PeerConnection& peer)
{
    const auto description = peer.localDescription();
    require(description.has_value(), "Missing local description after gathering");
    require(!description->candidates().empty(), "Loopback gathering produced no candidates");
    for (const auto& candidate : description->candidates()) {
        requireLoopback(candidate);
    }
    return *description;
}

std::string selectedPair(rtc::PeerConnection& peer)
{
    rtc::Candidate local;
    rtc::Candidate remote;
    require(peer.getSelectedCandidatePair(&local, &remote), "No selected ICE candidate pair");
    requireLoopback(local);
    requireLoopback(remote);
    local.resolve(rtc::Candidate::ResolveMode::Simple);
    remote.resolve(rtc::Candidate::ResolveMode::Simple);
    require(local.port().has_value() && remote.port().has_value(), "Selected pair has no ports");
    return "127.0.0.1:" + std::to_string(*local.port()) + " -> 127.0.0.1:" + std::to_string(*remote.port());
}

struct Received {
    Bytes bytes;
    std::uint32_t timestamp = 0;
};
struct Mailbox {
    std::mutex mutex;
    bool active = true;
    std::size_t maximum = 0;
    std::size_t ignoredAfterClose = 0;
    std::string error;
    std::vector<Received> frames;
};

class RoundTrip {
public:
    explicit RoundTrip(std::size_t frameLimit) : inbox(std::make_shared<Mailbox>())
    {
        inbox->maximum = frameLimit;
        rtc::Configuration configuration;
        configuration.bindAddress = "127.0.0.1";
        configuration.iceServers.clear();
        configuration.enableIceTcp = false;
        configuration.disableAutoNegotiation = true;
        configuration.disableAutoGathering = true;
        configuration.forceMediaTransport = true;
        configuration.disableFingerprintVerification = false;
        sender = std::make_shared<rtc::PeerConnection>(configuration);
        receiver = std::make_shared<rtc::PeerConnection>(configuration);
        rtc::Description::Video outgoing("trial", rtc::Description::Direction::SendOnly);
        outgoing.addH264Codec(payloadType);
        outgoing.addSSRC(videoSsrc, "qkvm-local-trial", "trial-stream", "trial-video");
        sendTrack = sender->addTrack(outgoing);
        auto packetConfig = std::make_shared<rtc::RtpPacketizationConfig>(videoSsrc, "qkvm-local-trial", payloadType,
                                                                          rtc::H264RtpPacketizer::ClockRate);
        // A deliberately small RTP payload exercises FU-A fragmentation with the generated clip.
        sendTrack->setMediaHandler(
            std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::LongStartSequence, packetConfig, 600));
        rtc::Description::Video incoming("trial", rtc::Description::Direction::RecvOnly);
        incoming.addH264Codec(payloadType);
        receiveTrack = receiver->addTrack(incoming);
        auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence);
        depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        receiveTrack->setMediaHandler(depacketizer);
        const std::weak_ptr<Mailbox> weak = inbox;
        receiveTrack->onFrame([weak](rtc::binary data, rtc::FrameInfo info) {
            const auto state = weak.lock();
            if (!state) {
                return;
            }
            std::lock_guard guard(state->mutex);
            if (!state->active) {
                ++state->ignoredAfterClose;
                return;
            }
            if (data.empty() || data.size() > maxUnitBytes || state->frames.size() >= state->maximum) {
                state->error = "Received frame exceeds the bounded trial inbox";
                return;
            }
            Bytes bytes(data.size());
            std::memcpy(bytes.data(), data.data(), data.size());
            state->frames.push_back({std::move(bytes), info.timestamp});
        });
        const auto onError = [weak](std::string error) {
            if (const auto state = weak.lock()) {
                std::lock_guard guard(state->mutex);
                if (state->active) {
                    state->error = std::move(error);
                }
            }
        };
        sendTrack->onError(onError);
        receiveTrack->onError(onError);
    }

    ~RoundTrip() { stop(); }
    RoundTrip(const RoundTrip&) = delete;
    RoundTrip& operator=(const RoundTrip&) = delete;

    template <class Predicate>
    void waitFor(Predicate complete, const char* operation)
    {
        const auto deadline = std::chrono::steady_clock::now() + operationTimeout;
        while (!complete()) {
            {
                std::lock_guard guard(inbox->mutex);
                require(inbox->error.empty(), inbox->error);
            }
            require(sender->state() != rtc::PeerConnection::State::Failed &&
                        receiver->state() != rtc::PeerConnection::State::Failed,
                    "Local peer connection failed");
            require(std::chrono::steady_clock::now() < deadline, std::string("Timed out: ") + operation);
            std::this_thread::sleep_for(5ms);
        }
    }

    void beginGathering()
    {
        sender->setLocalDescription(rtc::Description::Type::Offer);
        sender->gatherLocalCandidates();
    }

    void connect()
    {
        beginGathering();
        waitFor([&] { return sender->gatheringState() == rtc::PeerConnection::GatheringState::Complete; },
                "sender loopback gathering");
        receiver->setRemoteDescription(localDescription(*sender));
        receiver->setLocalDescription(rtc::Description::Type::Answer);
        receiver->gatherLocalCandidates();
        waitFor([&] { return receiver->gatheringState() == rtc::PeerConnection::GatheringState::Complete; },
                "receiver loopback gathering");
        sender->setRemoteDescription(localDescription(*receiver));
        waitFor([&] { return sendTrack->isOpen() && receiveTrack->isOpen(); }, "local media transport");
        std::cout << "selected_sender=" << selectedPair(*sender) << '\n';
        std::cout << "selected_receiver=" << selectedPair(*receiver) << '\n';
    }

    std::vector<Bytes> exchange(const std::vector<Bytes>& units)
    {
        for (std::size_t index = 0; index < units.size(); ++index) {
            const auto& unit = units[index];
            sendTrack->sendFrame(reinterpret_cast<const std::byte*>(unit.data()), unit.size(),
                                 rtc::FrameInfo(firstTimestamp + static_cast<std::uint32_t>(index) * timestampStep));
            std::this_thread::sleep_for(33ms);
        }
        waitFor(
            [&] {
                std::lock_guard guard(inbox->mutex);
                return inbox->frames.size() == units.size();
            },
            "H.264 frame reception");
        std::vector<Bytes> received;
        std::lock_guard guard(inbox->mutex);
        require(inbox->error.empty(), inbox->error);
        for (std::size_t index = 0; index < units.size(); ++index) {
            const auto& frame = inbox->frames[index];
            require(frame.timestamp == firstTimestamp + static_cast<std::uint32_t>(index) * timestampStep,
                    "RTP timestamp/order mismatch");
            Bytes canonical = canonicalAnnexB(frame.bytes);
            require(canonical == units[index], "Depacketized H.264 payload differs from sent access unit");
            received.push_back(std::move(canonical));
        }
        return received;
    }

    void stop() noexcept
    {
        if (stopped.exchange(true)) {
            return;
        }
        {
            std::lock_guard guard(inbox->mutex);
            inbox->active = false;
        }
        // No callback captures this RoundTrip, a peer, or a track. A late queued callback can
        // only find its old inactive inbox, never the next cycle's inbox or a dangling object.
        try {
            if (sendTrack) {
                sendTrack->resetCallbacks();
                sendTrack->close();
            }
            if (receiveTrack) {
                receiveTrack->onFrame(nullptr);
                receiveTrack->resetCallbacks();
                receiveTrack->close();
            }
            if (sender) {
                sender->resetCallbacks();
                sender->close();
            }
            if (receiver) {
                receiver->resetCallbacks();
                receiver->close();
            }
        } catch (...) {
            // Destruction is best effort; normal runs check peer closure separately below.
        }
    }

    void verifyClosed()
    {
        stop();
        const auto deadline = std::chrono::steady_clock::now() + operationTimeout;
        while (sender->state() != rtc::PeerConnection::State::Closed ||
               receiver->state() != rtc::PeerConnection::State::Closed) {
            require(std::chrono::steady_clock::now() < deadline, "Peer close timed out");
            std::this_thread::sleep_for(5ms);
        }
        const std::lock_guard guard(inbox->mutex);
        std::cout << "closed=true ignored_late_callbacks=" << inbox->ignoredAfterClose << '\n';
    }

private:
    std::shared_ptr<Mailbox> inbox;
    std::shared_ptr<rtc::PeerConnection> sender;
    std::shared_ptr<rtc::PeerConnection> receiver;
    std::shared_ptr<rtc::Track> sendTrack;
    std::shared_ptr<rtc::Track> receiveTrack;
    std::atomic<bool> stopped = false;
};

int boundedArgument(const char* text, int maximum)
{
    std::size_t consumed = 0;
    const std::string argument(text);
    const int value = std::stoi(argument, &consumed);
    require(consumed == argument.size() && value >= 1 && value <= maximum, "Argument outside allowed range");
    return value;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: qkvm_webrtc_cpp_trial fixture.h264 [cycles 1..20] [frames 1..120]\n";
        return 2;
    }
    int exitCode = 0;
    try {
        const int cycles = argc >= 3 ? boundedArgument(argv[2], 20) : 5;
        const int frames = argc >= 4 ? boundedArgument(argv[3], 120) : 30;
        const auto units = loadUnits(argv[1], static_cast<std::size_t>(frames));
        const auto largestNalBytes = largestNal(units);
        require(largestNalBytes > 600, "Fixture must contain a NAL larger than the RTP fragment limit");
        const auto reference = decode(units);
        require(!reference.empty(), "No decoded reference frames");
        rtc::InitLogger(rtc::LogLevel::Warning);
        rtc::SetThreadPoolSize(4);
        std::cout << "network=IPv4_loopback_only ice_servers=none fingerprint_verification=enabled\n"
                  << "fixture_units=" << units.size() << " reference_frames=" << reference.size()
                  << " largest_nal_bytes=" << largestNalBytes << " fragment_limit=600"
                  << " size=" << reference.front().width << 'x' << reference.front().height << '\n';
        for (int cycle = 0; cycle < cycles; ++cycle) {
            RoundTrip trial(units.size());
            const auto started = std::chrono::steady_clock::now();
            trial.connect();
            const auto received = trial.exchange(units);
            require(decode(received) == reference, "Received decoded images differ from reference decode");
            trial.verifyClosed();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                    .count();
            std::cout << "cycle=" << cycle + 1 << " matched_access_units=" << received.size()
                      << " matched_decoded_frames=" << reference.size() << " elapsed_ms=" << elapsed << '\n';
        }
        {
            RoundTrip cancelled(units.size());
            cancelled.beginGathering();
            cancelled.verifyClosed();
            std::cout << "close_after_gathering_started=passed\n";
        }
        std::cout << "result=PASS scope=local_transport_and_software_decode_only\n";
    } catch (const std::exception& error) {
        std::cerr << "result=FAIL reason=" << error.what() << '\n';
        exitCode = 1;
    }
    const auto cleanup = rtc::Cleanup();
    if (cleanup.wait_for(operationTimeout) != std::future_status::ready) {
        std::cerr << "result=FAIL reason=global_cleanup_timeout\n";
        std::cerr.flush();
        std::_Exit(3);
    }
    return exitCode;
}
