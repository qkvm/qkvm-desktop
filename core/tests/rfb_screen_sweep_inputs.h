#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QImage>

#include <cstdint>
#include <vector>

#include "rfb_by_hand.h"

// The inputs of the JPEG walk's exhaustive reading and of a real JPEG through the reader (test_rfb_screen_sweeps.cpp),
// which are also the first inputs of their fuzz targets (core/fuzz/make_seeds.cpp).
namespace qkvm::core::tests {

// A JPEG of Qt's own.
inline QByteArray jpegOf(const QImage& image, int quality = 100)
{
    QByteArray made;
    QBuffer buffer(&made);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", quality);
    return made;
}

// Real and hostile JPEGs: the first two real ones of Qt's (40 by 24 in colour, 16 by 8 in grey), then the shapes the
// reviews found the walk wanting at.
inline std::vector<QByteArray> jpegWalkCorpus()
{
    const auto seg = [](std::uint8_t marker, const QByteArray& payload) {
        QByteArray made("\xFF", 1);
        made.append(static_cast<char>(marker));
        const int length = static_cast<int>(payload.size()) + 2;
        made.append(static_cast<char>((length >> 8) & 0xFF));
        made.append(static_cast<char>(length & 0xFF));
        made.append(payload);
        return made;
    };
    const QByteArray soi("\xFF\xD8", 2);
    const QByteArray eoi("\xFF\xD9", 2);
    const QByteArray sofBody("\x08\x00\x10\x00\x10\x03\x01\x11\x00\x02\x11\x00\x03\x11\x00", 15);
    const QByteArray sos = seg(0xDA, QByteArray("\x01\x01\x00\x00\x3F\x00", 6));
    QImage image(40, 24, QImage::Format_RGB888);
    image.fill(QColor(10, 220, 130));
    QImage grey(16, 8, QImage::Format_Grayscale8);
    grey.fill(QColor(128, 128, 128));
    std::vector<QByteArray> corpus;
    corpus.push_back(jpegOf(image, 80)); // a real baseline JPEG (the single-byte sweep is of this one)
    corpus.push_back(jpegOf(grey, 80));
    corpus.push_back(soi + seg(0xC0, sofBody) + sos + QByteArray("\x12\x34", 2) + eoi);
    corpus.push_back(soi + seg(0xC2, sofBody) + sos + QByteArray("\x12\x34", 2) + eoi); // progressive
    corpus.push_back(QByteArray("\xFF\xD8\xFF\xC0\x00\x07\x08\x00\x10\x00\x10", 11));   // the HIGH's SOF of 7
    corpus.push_back(soi + seg(0xC0, sofBody) + QByteArray("\xFF\xDA\x00\x02", 4));     // the HIGH's SOS of 2
    corpus.push_back(soi + seg(0xDB, QByteArray(65, '\x00') + QByteArray("\xFF\xFE\x00\x40", 4)) + eoi); // DQT tail
    QByteArray jpgn = soi + seg(0xC0, sofBody) + sos + QByteArray("\x00", 1);
    for (int one = 0; one < 20; ++one) {
        jpgn += seg(0xF0, seg(0xC2, sofBody) + QByteArray(50, '\x11'));
    }
    jpgn += eoi;
    corpus.push_back(jpgn);
    return corpus;
}

// A real JPEG of 16 by 16 in colour, as a Tight JPEG rectangle sends it.
inline QByteArray realJpeg16()
{
    QImage image(16, 16, QImage::Format_RGB888);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            image.setPixel(x, y, qRgb(x * 16, y * 16, (x + y) * 8));
        }
    }
    return jpegOf(image, 80);
}

// The handshake of a server of 3.8, no password, a screen of 16 by 16.
inline rfb::tests::Bytes handshake16()
{
    namespace rfbt = rfb::tests;
    return rfbt::join({rfbt::greeting(), rfbt::securityTypes({1}), rfbt::word(0), rfbt::serverInit(16, 16, "s")});
}

// That handshake and one update of a Tight JPEG rectangle of `jpeg` over the whole screen.
inline rfb::tests::Bytes realJpegStream(const QByteArray& jpeg)
{
    namespace rfbt = rfb::tests;
    return rfbt::join(
        {handshake16(),
         rfbt::update({rfbt::tightJpeg(0, 0, 16, 16, std::vector<std::uint8_t>(jpeg.begin(), jpeg.end()))})});
}

} // namespace qkvm::core::tests
