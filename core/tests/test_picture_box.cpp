#include <QTest>
#include <QtEndian>
#include <qkvm/core/picture_box.h>

using qkvm::core::PictureBox;

namespace {
// RTP as at test_h264_rtp.cpp: one picture in one packet, its last byte its number in the film.
QByteArray packet(quint16 sequence, quint32 time, const QByteArray& payload)
{
    QByteArray bytes(12, '\0');
    bytes[0] = '\x80';
    bytes[1] = static_cast<char>(96 | 128);
    qToBigEndian(sequence, bytes.data() + 2);
    qToBigEndian(time, bytes.data() + 4);
    qToBigEndian<quint32>(123, bytes.data() + 8);
    return bytes + payload;
}

// A film of pictures sent 16 ms apart: a key frame (IDR) or one that builds on the one before.
struct Film {
    quint16 sequence = 1;
    quint32 time = 0;
    int number = 0;
    qint64 nowUs = 1'000'000;
    // The next picture, given to the box; true where the box says one waits.
    bool next(PictureBox& box, bool key)
    {
        time += 1500;
        nowUs += 16'000;
        QByteArray payload = QByteArray::fromHex(key ? "65" : "41");
        payload.append(static_cast<char>(number++));
        return box.packet(packet(sequence++, time, payload), nowUs);
    }
    void deltas(PictureBox& box, int count)
    {
        for (int each = 0; each < count; ++each) {
            next(box, false);
        }
    }
};

PictureBox madeBox(PictureBox::Pace pace = {})
{
    PictureBox box(96, pace);
    const bool taken = box.parameters(QByteArray::fromHex("6742e01faa"), QByteArray::fromHex("68ceaa"));
    Q_ASSERT(taken);
    return box;
}

// The numbers of what was taken, in order.
QList<int> numbers(const QList<PictureBox::Picture>& pictures)
{
    QList<int> said;
    for (const PictureBox::Picture& picture : pictures) {
        said.append(static_cast<unsigned char>(picture.bytes.back()));
    }
    return said;
}
} // namespace

class TestPictureBox : public QObject {
    Q_OBJECT
private slots:
    // As many as the engine lets wait are held, each with the time it became whole, and handed over
    // together.
    void eightWaitAndAllAreHanded()
    {
        PictureBox box = madeBox();
        Film film;
        QVERIFY(film.next(box, true));
        film.deltas(box, 7);
        const QList<PictureBox::Picture> taken = box.take();
        QCOMPARE(numbers(taken), (QList<int>{0, 1, 2, 3, 4, 5, 6, 7}));
        QVERIFY(taken[0].key);
        QVERIFY(taken[0].bytes.contains(QByteArray::fromHex("6742e01faa")));
        QVERIFY(!taken[1].key);
        QCOMPARE(taken[0].arrivedUs, 1'016'000);
        QCOMPARE(taken[7].arrivedUs, 1'128'000);
        QCOMPARE(box.lastWholeUs(), 1'128'000);
        QCOMPARE(box.counts().whole, quint64{8});
        QCOMPARE(box.counts().handed, quint64{8});
        QCOMPARE(box.counts().runsLost, quint64{0});
        QVERIFY(box.take().isEmpty());
    }
    // A ninth keeps the run from the newest key frame among those held, where that run and the ninth
    // are eight or fewer: what is held before that key frame is of no use to the decoder any more.
    void aNinthKeepsTheRunFromTheNewestKeyFrame()
    {
        PictureBox box = madeBox();
        Film film;
        film.next(box, true);
        film.deltas(box, 2);
        film.next(box, true);
        film.deltas(box, 4);
        film.next(box, false);
        QCOMPARE(numbers(box.take()), (QList<int>{3, 4, 5, 6, 7, 8}));
        QCOMPARE(box.counts().runsLost, quint64{0});
        QCOMPARE(box.counts().letGoBehind, quint64{3});
        QVERIFY(!box.lostAndAsks(film.nowUs));
        // And on from there.
        film.next(box, false);
        QCOMPARE(numbers(box.take()), (QList<int>{9}));
    }
    void aNinthThatIsAKeyFrameStandsAlone()
    {
        PictureBox box = madeBox();
        Film film;
        film.next(box, true);
        film.deltas(box, 7);
        film.next(box, true);
        QCOMPARE(numbers(box.take()), (QList<int>{8}));
        QCOMPARE(box.counts().runsLost, quint64{0});
        QCOMPARE(box.counts().letGoBehind, quint64{8});
        QVERIFY(!box.lostAndAsks(film.nowUs));
    }
    // Eight that build on a key frame handed over before them, with no key frame among them: a ninth
    // has no run to keep, so all go (never the oldest alone: what is left would not decode).
    void eightThatBuildOnAKeyFrameHandedEarlierAllGoAtANinth()
    {
        PictureBox box = madeBox();
        Film film;
        film.next(box, true);
        QCOMPARE(numbers(box.take()), (QList<int>{0}));
        film.deltas(box, 8);
        QVERIFY(!film.next(box, false));
        QVERIFY(box.take().isEmpty());
        QCOMPARE(box.counts().runsLost, quint64{1});
        QCOMPARE(box.counts().letGoBehind, quint64{9});
        film.next(box, false);
        QVERIFY(box.take().isEmpty());
        QVERIFY(film.next(box, true));
        QCOMPARE(numbers(box.take()), (QList<int>{11}));
    }
    // A ninth whose run from the newest key frame held is longer than eight: all go, the run is counted
    // lost, nothing is kept until the next key frame, and a key frame is asked for, once a second at most.
    void aNinthOutOfReachDropsAllCountsAndAsksOnceASecond()
    {
        PictureBox box = madeBox({8, 1000});
        Film film;
        film.next(box, true);
        film.deltas(box, 7);
        QVERIFY(!film.next(box, false));
        QVERIFY(box.take().isEmpty());
        QCOMPARE(box.counts().runsLost, quint64{1});
        QCOMPARE(box.counts().letGoBehind, quint64{9});
        const qint64 lost = film.nowUs;
        QVERIFY(box.lostAndAsks(lost));
        QVERIFY(!box.lostAndAsks(lost + 500'000));
        QVERIFY(!film.next(box, false));
        QVERIFY(box.take().isEmpty());
        QVERIFY(!box.lostAndAsks(lost + 999'000));
        QVERIFY(box.lostAndAsks(lost + 1'000'000));
        QCOMPARE(box.counts().keyAsks, quint64{2});
        QVERIFY(film.next(box, true));
        film.next(box, false);
        QCOMPARE(numbers(box.take()), (QList<int>{10, 11}));
        QVERIFY(!box.lostAndAsks(lost + 5'000'000));
        QCOMPARE(box.counts().runsLost, quint64{1});
    }
    // A packet the library hands over empty or larger than 64 KiB is a loss.
    void anEmptyOrOversizedPacketIsALoss()
    {
        for (const QByteArray& wrong : {QByteArray(), QByteArray(65537, '\x80')}) {
            PictureBox box = madeBox();
            Film film;
            film.next(box, true);
            QCOMPARE(box.take().size(), 1);
            QVERIFY(!box.packet(wrong, film.nowUs));
            film.next(box, false);
            QVERIFY(box.take().isEmpty());
            QVERIFY(box.lostAndAsks(film.nowUs));
        }
    }
    // A card only (design 4): what is no key frame is let go as it comes, and that is no loss. A window
    // that comes then goes on from the next key frame: what came between was let go.
    void cardOnlyLetsGoOfTheRestUncounted()
    {
        PictureBox box = madeBox();
        Film film;
        box.keyFramesOnly(true);
        film.next(box, true);
        film.deltas(box, 2);
        film.next(box, true);
        film.deltas(box, 20);
        QCOMPARE(numbers(box.take()), (QList<int>{0, 3}));
        QCOMPARE(box.counts().letGoAsCardOnly, quint64{22});
        QCOMPARE(box.counts().runsLost, quint64{0});
        QCOMPARE(box.counts().whole, quint64{24});
        QCOMPARE(box.lastWholeUs(), film.nowUs);
        QVERIFY(!box.lostAndAsks(film.nowUs));
        box.keyFramesOnly(false);
        film.deltas(box, 1);
        QVERIFY(box.take().isEmpty());
        film.next(box, true);
        film.deltas(box, 1);
        QCOMPARE(numbers(box.take()), (QList<int>{25, 26}));
    }
    // F2: a window that opens gets every picture from the next key frame, never from one held before.
    void fromTheNextKeyFrameNeverOneHeldBefore()
    {
        PictureBox box = madeBox();
        Film film;
        film.next(box, true);
        film.deltas(box, 2);
        box.fromTheNextKeyFrame();
        QVERIFY(box.take().isEmpty());
        QVERIFY(!film.next(box, false));
        QVERIFY(box.take().isEmpty());
        QVERIFY(film.next(box, true));
        film.next(box, false);
        QCOMPARE(numbers(box.take()), (QList<int>{4, 5}));
        QCOMPARE(box.counts().runsLost, quint64{0});
    }
    // M2: a window's wish for a key frame throws nothing away; it may be asked once a second at most,
    // and a loss's asking keeps to the same pace.
    void askingThrowsNothingAway()
    {
        PictureBox box = madeBox({8, 1000});
        Film film;
        film.next(box, true);
        film.deltas(box, 2);
        const qint64 asked = film.nowUs;
        QVERIFY(box.mayAsk(asked));
        QVERIFY(!box.mayAsk(asked + 100'000));
        film.next(box, false);
        QCOMPARE(numbers(box.take()), (QList<int>{0, 1, 2, 3}));
        film.next(box, false);
        QCOMPARE(numbers(box.take()), (QList<int>{4}));
        QVERIFY(!box.mayAsk(asked + 999'000));
        QVERIFY(box.mayAsk(asked + 1'000'000));
        QCOMPARE(box.counts().keyAsks, quint64{2});
        QCOMPARE(box.counts().runsLost, quint64{0});
        // A loss right after a window's asking waits out the same second.
        QVERIFY(!box.packet({}, asked + 1'010'000));
        QVERIFY(!box.lostAndAsks(asked + 1'500'000));
        QVERIFY(box.lostAndAsks(asked + 2'000'000));
    }
    // A window's wish that the pace refused is not forgotten (2b, after its reading: a window opened
    // within the pace of a loss's asking stayed blank until the device's own next key frame): it is
    // asked once the pace has passed, at the next packet, once; and a key frame that comes meanwhile
    // answers it.
    void aWishThePaceRefusedIsAskedOnceThePaceHasPassed()
    {
        PictureBox box = madeBox({8, 1000});
        Film film;
        film.next(box, true);
        film.deltas(box, 1);
        const qint64 asked = film.nowUs;
        QVERIFY(box.mayAsk(asked));
        QVERIFY(!box.mayAsk(asked + 200'000));
        // (For a timer of its owner's, where no packet comes: 2c, the second reading.)
        QCOMPARE(box.paceLeftUs(asked + 200'000), 800'000);
        QVERIFY(!box.wishAsks(asked + 250'000));
        QVERIFY(!box.lostAndAsks(asked + 300'000));
        QVERIFY(!box.lostAndAsks(asked + 999'000));
        QVERIFY(box.lostAndAsks(asked + 1'000'000));
        QVERIFY(!box.lostAndAsks(asked + 2'100'000));
        QCOMPARE(box.counts().keyAsks, quint64{2});
        QCOMPARE(box.paceLeftUs(asked + 2'100'000), 0);
        QVERIFY(!box.wishAsks(asked + 2'100'000));
        // A wish refused again, answered by a key frame that came meanwhile: nothing more is asked.
        QVERIFY(box.mayAsk(asked + 2'200'000));
        QVERIFY(!box.mayAsk(asked + 2'300'000));
        film.nowUs = asked + 2'400'000;
        film.next(box, true);
        QVERIFY(!box.lostAndAsks(asked + 3'500'000));
        QCOMPARE(box.counts().keyAsks, quint64{3});
        QCOMPARE(box.counts().runsLost, quint64{0});
        QCOMPARE(numbers(box.take()), (QList<int>{0, 1, 2}));
    }
    // Waiting for the next key frame (a window that opened, or a card's connection shown large again)
    // asks for one at the box's pace, and stops once one came.
    void waitingForTheNextKeyFrameAsksForOne()
    {
        PictureBox box = madeBox({8, 1000});
        Film film;
        film.next(box, true);
        QCOMPARE(box.take().size(), 1);
        QVERIFY(!box.lostAndAsks(film.nowUs));
        box.fromTheNextKeyFrame();
        QVERIFY(box.lostAndAsks(film.nowUs));
        QVERIFY(!box.lostAndAsks(film.nowUs + 500'000));
        QVERIFY(box.lostAndAsks(film.nowUs + 1'000'000));
        film.nowUs += 1'100'000;
        film.next(box, true);
        QVERIFY(!box.lostAndAsks(film.nowUs + 5'000'000));
        box.keyFramesOnly(true);
        box.keyFramesOnly(false);
        QVERIFY(box.lostAndAsks(film.nowUs + 6'000'000));
    }
};
QTEST_GUILESS_MAIN(TestPictureBox)
#include "test_picture_box.moc"
