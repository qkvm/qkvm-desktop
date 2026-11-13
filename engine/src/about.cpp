#include <qkvm/engine/about.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

namespace qkvm::engine {

DecoderLibrary decoderLibrary()
{
    return {av_version_info(), avutil_license(), avutil_configuration()};
}

DecoderAbilities decoderAbilities()
{
    DecoderAbilities abilities;
    const AVCodec* h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
    abilities.h264 = h264 != nullptr;
    abilities.jpeg = avcodec_find_decoder(AV_CODEC_ID_MJPEG) != nullptr;
    abilities.png = avcodec_find_decoder(AV_CODEC_ID_PNG) != nullptr;
    if (h264 != nullptr) {
        // The decoder lists the ways it can be handed a graphics device; the list ends with a null.
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* way = avcodec_get_hw_config(h264, index);
            if (way == nullptr) {
                break;
            }
            if (way->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
                (way->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                abilities.h264OnTheCard = true;
            }
        }
    }
    return abilities;
}

} // namespace qkvm::engine
