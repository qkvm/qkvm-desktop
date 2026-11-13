#pragma once

// FFmpeg, for the engine's own files, with its objects owned by scope: nothing in the engine calls
// one of its free functions by hand. Nothing of this shows in the engine's headers.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <memory>
#include <string>

namespace qkvm::engine {

struct FreeFrame {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
struct FreePacket {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};
struct FreeCodecContext {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};
struct FreeBuffer {
    void operator()(AVBufferRef* buffer) const noexcept { av_buffer_unref(&buffer); }
};
struct FreeParser {
    void operator()(AVCodecParserContext* parser) const noexcept { av_parser_close(parser); }
};

using FramePtr = std::unique_ptr<AVFrame, FreeFrame>;
using PacketPtr = std::unique_ptr<AVPacket, FreePacket>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, FreeCodecContext>;
using BufferPtr = std::unique_ptr<AVBufferRef, FreeBuffer>;
using ParserPtr = std::unique_ptr<AVCodecParserContext, FreeParser>;

// FFmpeg's number for a failure, in its own words.
std::string ffmpegSays(int result);

// FFmpeg writes what it has to say to the standard error stream, from every thread, and a stream
// nobody reads can make a writer wait. It is told once to keep it to itself; how many errors it
// had is counted instead.
void quietFfmpeg();
std::uint64_t ffmpegErrors() noexcept;

} // namespace qkvm::engine
