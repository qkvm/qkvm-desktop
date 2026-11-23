// A card's still made smaller (PERF-2, stream_core.cpp's shrunkTo), by itself: what the scene's tests cannot hand in
// through a stream, a picture that says nothing of its colour standard. (It is the engine's own, in src, with
// FFmpeg's headers: as test_decode_pool is.)

#include <cstring>

#include <QTest>

#include "stream_core.h"

using namespace qkvm;

class TestShrink : public QObject {
    Q_OBJECT

private slots:
    // A picture made smaller that says nothing of its colour standard keeps the one its own size had it read by
    // (colourRowsFor takes the high-definition one from 577 lines on, but for the whole range of a JPEG): its colours
    // are the same either way. (Made smaller to 135 lines and read as such, a 1080p picture's were the small-screen
    // standard's.)
    void aShrunkPictureThatSaysNoColourStandardKeepsTheOneOfItsSize()
    {
        struct Kind {
            int width;
            int height;
            AVPixelFormat format;
        };
        for (const Kind kind : {Kind{1920, 1080, AV_PIX_FMT_YUV420P}, Kind{720, 576, AV_PIX_FMT_YUV420P},
                                Kind{1920, 1080, AV_PIX_FMT_YUVJ420P}}) {
            const engine::FramePtr whole(av_frame_alloc());
            QVERIFY(whole != nullptr);
            whole->format = kind.format;
            whole->width = kind.width;
            whole->height = kind.height;
            QVERIFY(av_frame_get_buffer(whole.get(), 0) >= 0);
            whole->colorspace = AVCOL_SPC_UNSPECIFIED;
            for (int plane = 0; plane < 3; ++plane) {
                const int rows = plane == 0 ? kind.height : (kind.height + 1) / 2;
                std::memset(whole->data[plane], plane == 0 ? 90 : 160,
                            static_cast<std::size_t>(whole->linesize[plane]) * static_cast<std::size_t>(rows));
            }
            const engine::FramePtr smaller = engine::shrunkTo(*whole, {kind.width / 8, kind.height / 8});
            QVERIFY(smaller != nullptr);
            QCOMPARE(smaller->width, kind.width / 8);
            QCOMPARE(smaller->height, kind.height / 8);
            // (Each value the mean of those under it: the same here, everywhere.)
            QCOMPARE(smaller->data[0][0], std::uint8_t{90});
            QCOMPARE(smaller->data[1][0], std::uint8_t{160});
            const engine::ColourRows before = engine::colourRowsFor(*whole);
            const engine::ColourRows after = engine::colourRowsFor(*smaller);
            for (std::size_t at = 0; at < 4; ++at) {
                QVERIFY2(before.toRed[at] == after.toRed[at] && before.toGreen[at] == after.toGreen[at] &&
                             before.toBlue[at] == after.toBlue[at],
                         qPrintable(QStringLiteral("%1 x %2").arg(kind.width).arg(kind.height)));
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestShrink)

#include "test_shrink.moc"
