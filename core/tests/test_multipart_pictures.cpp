#include <algorithm>
#include <optional>

#include <QByteArray>
#include <QList>
#include <QTest>

#include <qkvm/core/kvmd_answers.h>
#include <qkvm/core/multipart_pictures.h>

using namespace Qt::StringLiterals;
using qkvm::core::boundaryFrom;
using qkvm::core::MultipartPictures;
using End = qkvm::core::MultipartPictures::End;
using Limits = qkvm::core::MultipartPictures::Limits;

Q_DECLARE_METATYPE(End)

namespace {

// The word ustreamer puts between its pictures.
QByteArray word()
{
    return "boundarydonotcross"_ba;
}

QByteArray boundary()
{
    return "--" + word() + "\r\n";
}

// One part with lines of the test's own: the boundary, the lines, the empty one, the bytes, a line end.
QByteArray partWith(const QByteArray& lines, const QByteArray& bytes)
{
    return boundary() + lines + "\r\n" + bytes + "\r\n";
}

// One part as ustreamer writes it.
QByteArray part(const QByteArray& bytes)
{
    return partWith("Content-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(bytes.size()) +
                        "\r\nX-Timestamp: 1728000000.123456\r\n",
                    bytes);
}

// Something that begins as a JPEG does.
QByteArray picture(char fill, qsizetype size = 40)
{
    return QByteArray("\xff\xd8\xff\xe0", 4) + QByteArray(size - 4, fill);
}

struct Read {
    QList<QByteArray> pictures;
    End end = End::None;
    qsizetype taken = 0;    // how many bytes the reader took in all
    qsizetype mostRoom = 0; // the most memory it held at any time
    bool stuck = false;     // it took nothing, handed out nothing and was not over: never
};

// Feeds the pieces as whoever reads a line does: on with the rest after every picture, until
// there is nothing left or the reading is over.
Read readIn(MultipartPictures& reader, const QList<QByteArray>& pieces)
{
    Read read;
    for (const QByteArray& piece : pieces) {
        QByteArrayView rest(piece);
        while (!rest.isEmpty() && reader.end() == End::None && !read.stuck) {
            const qsizetype taken = reader.feed(rest);
            rest = rest.sliced(taken);
            read.taken += taken;
            read.mostRoom = std::max(read.mostRoom, reader.room());
            if (std::optional<QByteArray> whole = reader.next()) {
                read.pictures.append(*whole);
            } else {
                read.stuck = taken == 0 && reader.end() == End::None;
            }
        }
    }
    read.end = reader.end();
    return read;
}

Read readWhole(const QByteArray& stream, Limits limits = {})
{
    MultipartPictures reader(word(), limits);
    return readIn(reader, {stream});
}

} // namespace

// A KVM's stream of pictures, read: with whatever a device might send.
class TestMultipartPictures : public QObject {
    Q_OBJECT

private slots:
    void theWordIsReadFromWhatTheAnswerCallsItself_data()
    {
        QTest::addColumn<QByteArray>("kind");
        QTest::addColumn<QByteArray>("found"); // null: none
        const QByteArray none;
        const QByteArray seventy(70, 'w');
        QTest::newRow("as ustreamer says it") << "multipart/x-mixed-replace;boundary=boundarydonotcross"_ba << word();
        QTest::newRow("with a blank") << "multipart/x-mixed-replace; boundary=abc"_ba << "abc"_ba;
        QTest::newRow("in any case") << "Multipart/X-Mixed-Replace; BOUNDARY=aBc"_ba << "aBc"_ba;
        QTest::newRow("blanks around it") << "  multipart/x-mixed-replace ;\tboundary=abc  "_ba << "abc"_ba;
        QTest::newRow("in quotes") << "multipart/x-mixed-replace; boundary=\"abc\""_ba << "abc"_ba;
        QTest::newRow("a blank inside quotes") << "multipart/x-mixed-replace; boundary=\"a b\""_ba << "a b"_ba;
        QTest::newRow("every other sign") << "multipart/x-mixed-replace; boundary=\"'()+_,-./:=?\""_ba
                                          << "'()+_,-./:=?"_ba;
        QTest::newRow("those without quotes") << "multipart/x-mixed-replace; boundary='()+_,-./:=?"_ba
                                              << "'()+_,-./:=?"_ba;
        QTest::newRow("among others") << "multipart/x-mixed-replace; a=b; boundary=abc; c=\"d;boundary=e\""_ba
                                      << "abc"_ba;
        QTest::newRow("seventy signs") << "multipart/x-mixed-replace; boundary=" + seventy << seventy;

        QTest::newRow("seventy-one") << "multipart/x-mixed-replace; boundary=" + seventy + "w" << none;
        QTest::newRow("no word") << "multipart/x-mixed-replace"_ba << none;
        QTest::newRow("an empty word") << "multipart/x-mixed-replace; boundary="_ba << none;
        QTest::newRow("an empty word in quotes") << "multipart/x-mixed-replace; boundary=\"\""_ba << none;
        QTest::newRow("named twice") << "multipart/x-mixed-replace; boundary=abc; boundary=abc"_ba << none;
        QTest::newRow("a blank at its end") << "multipart/x-mixed-replace; boundary=\"abc \""_ba << none;
        QTest::newRow("a blank without quotes") << "multipart/x-mixed-replace; boundary=a b"_ba << none;
        QTest::newRow("a sign that is none of its") << "multipart/x-mixed-replace; boundary=a*c"_ba << none;
        QTest::newRow("a line end in it") << "multipart/x-mixed-replace; boundary=\"a\r\nc\""_ba << none;
        QTest::newRow("a backslash in it") << "multipart/x-mixed-replace; boundary=\"a\\\\c\""_ba << none;
        QTest::newRow("a quote in it") << "multipart/x-mixed-replace; boundary=a\"c"_ba << none;
        QTest::newRow("quotes never closed") << "multipart/x-mixed-replace; boundary=\"abc"_ba << none;
        QTest::newRow("blanks around the equals sign") << "multipart/x-mixed-replace; boundary = abc"_ba << none;
        QTest::newRow("nothing after the semicolon") << "multipart/x-mixed-replace; boundary=abc;"_ba << none;
        QTest::newRow("something that is no parameter") << "multipart/x-mixed-replace; boundary=abc; oops"_ba << none;
        QTest::newRow("no semicolon") << "multipart/x-mixed-replace boundary=abc"_ba << none;
        QTest::newRow("another kind of several parts") << "multipart/mixed; boundary=abc"_ba << none;
        QTest::newRow("a kind that only begins so") << "multipart/x-mixed-replaced; boundary=abc"_ba << none;
        QTest::newRow("another kind as long as it") << "multipart/x-fixed-replace; boundary=abc"_ba << none;
        QTest::newRow("a picture") << "image/jpeg"_ba << none;
        QTest::newRow("nothing") << QByteArray() << none;
        QTest::newRow("longer than any KVM says it")
            << "multipart/x-mixed-replace; boundary=abc; x=" + QByteArray(2000, 'y') << none;
    }

    void theWordIsReadFromWhatTheAnswerCallsItself()
    {
        QFETCH(QByteArray, kind);
        QFETCH(QByteArray, found);
        const std::optional<QByteArray> read = boundaryFrom(kind);
        QCOMPARE(read.has_value(), !found.isNull());
        QCOMPARE(read.value_or(QByteArray()), found);
    }

    void picturesComeOutInTheOrderTheyCame()
    {
        const QByteArray first = picture('a');
        const QByteArray second = picture('b', 3000);
        const QByteArray third = picture('c', 1);
        const Read read = readWhole(part(first) + part(second) + part(third) + boundary());
        QCOMPARE(read.end, End::None);
        QCOMPARE(read.pictures, (QList<QByteArray>{first, second, third}));
        QVERIFY(!read.stuck);
    }

    void everyWayOfCuttingAStreamGivesTheSamePictures()
    {
        const QByteArray first = picture('a', 23);
        const QByteArray second = picture('b', 31);
        const QList<QByteArray> both{first, second};
        // As ustreamer writes it, with the boundary of a third picture behind the second; and
        // one that is closed after the second.
        for (const QByteArray& last : {boundary(), "--" + word() + "--\r\n"}) {
            const QByteArray stream = part(first) + part(second) + last;
            const End expected = last == boundary() ? End::None : End::Closed;

            MultipartPictures byteByByte(word());
            QList<QByteArray> bytes;
            for (const char byte : stream) {
                bytes.append(QByteArray(1, byte));
            }
            const Read slowly = readIn(byteByByte, bytes);
            QCOMPARE(slowly.pictures, both);
            QCOMPARE(slowly.end, expected);

            // Cut in two at every place, and in three at every pair of places.
            for (qsizetype cut = 0; cut <= stream.size(); ++cut) {
                MultipartPictures inTwo(word());
                const Read read = readIn(inTwo, {stream.first(cut), stream.sliced(cut)});
                if (read.pictures != both || read.end != expected || read.stuck) {
                    QFAIL(qPrintable(u"cut at %1"_s.arg(cut)));
                }
                for (qsizetype next = cut; next <= stream.size(); ++next) {
                    MultipartPictures inThree(word());
                    const Read three =
                        readIn(inThree, {stream.first(cut), stream.sliced(cut, next - cut), stream.sliced(next)});
                    if (three.pictures != both || three.end != expected || three.stuck) {
                        QFAIL(qPrintable(u"cut at %1 and %2"_s.arg(cut).arg(next)));
                    }
                }
            }
        }
    }

    void whatAPartCarriesIsHandedOnAsItCame()
    {
        // Whether it is a picture is the decoder's to say: here it is N bytes. So what begins as
        // a JPEG is handed on, and what does not, and what the part calls itself changes nothing.
        const QByteArray jpeg = picture('j');
        const QByteArray none = "this is no picture at all"_ba;
        // The bytes are counted, not searched: a boundary inside them is bytes like the others.
        const QByteArray tricky = "\r\n" + boundary() + "Content-Length: 5\r\n\r\n" + QByteArray("\x00\xff\r\n\x00", 5);
        const QByteArray stream =
            part(jpeg) + part(none) +
            partWith("Content-Type: text/plain\r\nContent-Length: " + QByteArray::number(tricky.size()) + "\r\n",
                     tricky) +
            boundary();
        const Read read = readWhole(stream);
        QCOMPARE(read.end, End::None);
        QCOMPARE(read.pictures, (QList<QByteArray>{jpeg, none, tricky}));
    }

    void theLengthIsFoundAmongTheOtherLines_data()
    {
        QTest::addColumn<QByteArray>("lines");
        QTest::newRow("alone") << "Content-Length: 5\r\n"_ba;
        QTest::newRow("in small letters") << "content-length: 5\r\n"_ba;
        QTest::newRow("in capitals") << "CONTENT-LENGTH: 5\r\n"_ba;
        QTest::newRow("no blank before it") << "Content-Length:5\r\n"_ba;
        QTest::newRow("blanks and tabs around it") << "Content-Length: \t 5 \t\r\n"_ba;
        QTest::newRow("zeros before it") << "Content-Length: 0005\r\n"_ba;
        QTest::newRow("last") << "Content-Type: image/jpeg\r\nX-Timestamp: 1.5\r\nContent-Length: 5\r\n"_ba;
        QTest::newRow("among lines that look like it")
            << "Content-Lengthy: 9\r\nX-Content-Length: 9\r\nContent-Length : 9\r\nContent-Length: 5\r\n"_ba;
        QTest::newRow("after a line that is none")
            << "no colon in this one\r\n: nor a name\r\nContent-Length: 5\r\n"_ba;
    }

    void theLengthIsFoundAmongTheOtherLines()
    {
        QFETCH(QByteArray, lines);
        const Read read = readWhole(partWith(lines, "12345"_ba) + boundary());
        QCOMPARE(read.end, End::None);
        QCOMPARE(read.pictures, (QList<QByteArray>{"12345"_ba}));
    }

    void aLengthThatIsNoneEndsIt_data()
    {
        QTest::addColumn<QByteArray>("lines");
        QTest::addColumn<End>("why");
        const auto stated = [](const QByteArray& length) { return "Content-Length: " + length + "\r\n"; };
        QTest::newRow("no length at all") << "Content-Type: image/jpeg\r\n"_ba << End::NoLength;
        QTest::newRow("no lines at all") << QByteArray() << End::NoLength;
        QTest::newRow("nothing") << stated(""_ba) << End::BadLength;
        QTest::newRow("0") << stated("0"_ba) << End::BadLength;
        QTest::newRow("many zeros") << stated("0000"_ba) << End::BadLength;
        QTest::newRow("-1") << stated("-1"_ba) << End::BadLength;
        QTest::newRow("+1") << stated("+1"_ba) << End::BadLength;
        QTest::newRow("letters") << stated("many"_ba) << End::BadLength;
        QTest::newRow("a number and a letter") << stated("1x"_ba) << End::BadLength;
        QTest::newRow("in sixteens") << stated("0x10"_ba) << End::BadLength;
        QTest::newRow("with a point") << stated("1.0"_ba) << End::BadLength;
        QTest::newRow("two numbers") << stated("1 1"_ba) << End::BadLength;
        QTest::newRow("two numbers and a comma") << stated("1, 1"_ba) << End::BadLength;
        QTest::newRow("a blank of another kind") << stated("\v1"_ba) << End::BadLength;
        QTest::newRow("said twice") << stated("1"_ba) + stated("1"_ba) << End::BadLength;
        QTest::newRow("said twice, differently") << stated("1"_ba) + "content-length: 2\r\n" << End::BadLength;
        // Ten digits that are 1 to whoever counts in thirty-two bits, and more digits than any
        // number holds.
        QTest::newRow("ten digits") << stated("4294967297"_ba) << End::TooLarge;
        QTest::newRow("digits without end") << stated(QByteArray(3000, '9')) << End::TooLarge;
        QTest::newRow("one more than is taken")
            << stated(QByteArray::number(qkvm::core::kvmd::largestPictureBytes + 1)) << End::TooLarge;
    }

    void aLengthThatIsNoneEndsIt()
    {
        QFETCH(QByteArray, lines);
        QFETCH(End, why);
        // (What follows would be a picture of one byte to whoever read the length wrongly.)
        const Read read = readWhole(part("before"_ba) + partWith(lines, "1"_ba) + part("after"_ba) + boundary());
        QCOMPARE(read.end, why);
        QCOMPARE(read.pictures, (QList<QByteArray>{"before"_ba}));
        QVERIFY(!read.stuck);
    }

    void aPictureLargerThanIsTakenEndsItBeforeAByteOfItIsTaken()
    {
        const QByteArray head =
            boundary() + "Content-Length: " + QByteArray::number(qkvm::core::kvmd::largestPictureBytes + 1) + "\r\n";
        MultipartPictures reader(word());
        const Read read = readIn(reader, {head + "\r\n" + QByteArray(100'000, 'x')});
        QCOMPARE(read.end, End::TooLarge);
        QVERIFY(read.pictures.isEmpty());
        // It ended where the length stood, and never held more than the lines of that part.
        QCOMPARE(read.taken, head.size());
        QVERIFY(read.mostRoom <= 8192);
        QCOMPARE(reader.room(), 0);
    }

    void aStatedLengthTakesNoMemoryByItself()
    {
        // The largest picture that is taken, stated, and three bytes of it sent: what the reader
        // holds is a first piece of room, not thirty-two megabytes on a KVM's word.
        const qsizetype largest = qkvm::core::kvmd::largestPictureBytes;
        MultipartPictures reader(word());
        const Read read =
            readIn(reader, {boundary() + "Content-Length: " + QByteArray::number(largest) + "\r\n\r\nabc"});
        QCOMPARE(read.end, End::None);
        QVERIFY(read.pictures.isEmpty());
        QVERIFY2(read.mostRoom <= qsizetype{300} * 1024, qPrintable(QString::number(read.mostRoom)));

        // And as it comes, never more room than was stated; then it is handed on whole.
        const QByteArray megabyte(qsizetype{1024} * 1024, 'm');
        qsizetype mostRoom = 0;
        qsizetype sent = 3;
        while (sent < largest) {
            const qsizetype now = std::min<qsizetype>(megabyte.size(), largest - sent);
            QCOMPARE(reader.feed(QByteArrayView(megabyte).first(now)), now);
            mostRoom = std::max(mostRoom, reader.room());
            sent += now;
        }
        QVERIFY2(mostRoom <= largest + 8192, qPrintable(QString::number(mostRoom)));
        const QByteArray whole = reader.next().value_or(QByteArray());
        QCOMPARE(whole.size(), largest);
        QVERIFY(whole.startsWith("abcmmm"));
        QVERIFY(whole.capacity() <= largest + 16);
        QCOMPARE(reader.end(), End::None);
        // Handed out, it is no longer held here.
        QVERIFY(reader.room() <= 8192);
    }

    void aLimitIsOnlyEverMadeSmaller()
    {
        Limits generous;
        generous.headerBytes = 1'000'000;
        generous.pictureBytes = qsizetype{4} * 1024 * 1024 * 1024;
        const QByteArray tooLarge = boundary() +
                                    "Content-Length: " + QByteArray::number(qkvm::core::kvmd::largestPictureBytes + 1) +
                                    "\r\n\r\n";
        QCOMPARE(readWhole(tooLarge, generous).end, End::TooLarge);
        QCOMPARE(readWhole(boundary() + "X: " + QByteArray(5000, 'y'), generous).end, End::HeadersTooLong);

        Limits small;
        small.pictureBytes = 10;
        QCOMPARE(readWhole(part("0123456789"_ba) + boundary(), small).pictures.size(), 1);
        QCOMPARE(readWhole(part("0123456789a"_ba) + boundary(), small).end, End::TooLarge);
    }

    void headerLinesWithoutEndEndIt()
    {
        // One line that never ends.
        MultipartPictures one(word());
        const Read endless = readIn(one, {boundary() + "X: " + QByteArray(100'000, 'y')});
        QCOMPARE(endless.end, End::HeadersTooLong);
        QCOMPARE(endless.taken, boundary().size() + 4097);
        QVERIFY2(endless.mostRoom <= 16384, qPrintable(QString::number(endless.mostRoom)));
        QCOMPARE(one.room(), 0);

        // Lines without end, each of them short.
        QByteArray many = boundary();
        for (int line = 0; line < 5000; ++line) {
            many += "X-Line: " + QByteArray::number(line) + "\r\n";
        }
        MultipartPictures other(word());
        const Read lines = readIn(other, {many});
        QCOMPARE(lines.end, End::HeadersTooLong);
        QCOMPARE(lines.taken, boundary().size() + 4097);

        // Exactly as much as is read, the empty line included, is read; a byte more is not.
        const QByteArray length = "Content-Length: 3\r\n"_ba;
        const auto filled = [&length](qsizetype bytes) {
            return length + "X: " + QByteArray(bytes - length.size() - 3 - 4, 'y') + "\r\n";
        };
        QCOMPARE(filled(4096).size() + 2, 4096);
        const Read just = readWhole(partWith(filled(4096), "abc"_ba) + boundary());
        QCOMPARE(just.end, End::None);
        QCOMPARE(just.pictures, (QList<QByteArray>{"abc"_ba}));
        QCOMPARE(readWhole(partWith(filled(4097), "abc"_ba) + boundary()).end, End::HeadersTooLong);
    }

    void aLineThatDoesNotEndAsLinesDoEndsIt_data()
    {
        QTest::addColumn<QByteArray>("stream");
        QTest::addColumn<int>("pictures"); // that were whole before it
        const QByteArray good = part("good"_ba);
        const QByteArray lines = "Content-Type: image/jpeg\r\nContent-Length: 4\r\n"_ba;
        QByteArray allBare = good + part("more"_ba) + boundary();
        allBare.replace("\r\n", "\n");
        QTest::newRow("every line of the stream") << allBare << 0;
        QTest::newRow("the first boundary") << "--" + word() + "\n" + lines + "\r\nbare\r\n" + boundary() << 0;
        QTest::newRow("a later boundary") << good + "--" + word() + "\n" + lines + "\r\nbare\r\n" + boundary() << 1;
        QTest::newRow("a line of a part")
            << good + boundary() + "Content-Type: image/jpeg\nContent-Length: 4\r\n\r\nbare\r\n" + boundary() << 1;
        QTest::newRow("the line that says the length")
            << good + boundary() + "Content-Length: 4\n\r\nbare\r\n" + boundary() << 1;
        QTest::newRow("the empty line") << good + boundary() + lines + "\nbare\r\n" + boundary() << 1;
        QTest::newRow("the line end behind a picture") << good + boundary() + lines + "\r\nbare\n" + boundary() << 2;
        QTest::newRow("the closing boundary") << good + "--" + word() + "-\n" << 1;
        // And a CR that no LF follows.
        QTest::newRow("a CR alone in a line") << good + boundary() + "Content-Length: 4\rX\r\n\r\nbare\r\n" << 1;
        QTest::newRow("a CR alone behind the boundary") << good + "--" + word() + "\rX" + lines + "\r\nbare\r\n" << 1;
        QTest::newRow("a CR alone behind a picture") << good + boundary() + lines + "\r\nbare\r" + boundary() << 2;
        QTest::newRow("a CR alone for the empty line") << good + boundary() + lines + "\rbare\r\n" + boundary() << 1;
    }

    void aLineThatDoesNotEndAsLinesDoEndsIt()
    {
        QFETCH(QByteArray, stream);
        QFETCH(int, pictures);
        const Read read = readWhole(stream);
        QCOMPARE(read.end, End::BadLineEnd);
        QCOMPARE(read.pictures.size(), pictures);
        QVERIFY(!read.stuck);
    }

    void whatFollowsAPictureAndIsNotTheBoundaryEndsIt_data()
    {
        QTest::addColumn<QByteArray>("after");
        const QByteArray lines = "Content-Length: 4\r\n\r\nnext\r\n"_ba;
        QTest::newRow("another word") << "--anotherword\r\n" + lines;
        QTest::newRow("a word that begins as it") << "--" + word() + "s\r\n" + lines;
        QTest::newRow("a word that is its beginning") << "--boundarydonot\r\n" + lines;
        QTest::newRow("in other letters") << "--" + word().toUpper() + "\r\n" + lines;
        QTest::newRow("one dash") << "-" + word() + "\r\n" + lines;
        QTest::newRow("a blank behind it") << "--" + word() + " \r\n" + lines;
        QTest::newRow("one dash behind it") << "--" + word() + "-x\r\n" + lines;
        QTest::newRow("the next part at once") << lines;
        QTest::newRow("an empty line first") << "\r\n" + boundary() + lines;
        QTest::newRow("more of the picture than it stated") << "xx\r\n" + boundary() + lines;
    }

    void whatFollowsAPictureAndIsNotTheBoundaryEndsIt()
    {
        QFETCH(QByteArray, after);
        const Read read = readWhole(part("first"_ba) + after + boundary());
        QCOMPARE(read.end, End::WrongBoundary);
        // The picture that was whole by then had been handed on.
        QCOMPARE(read.pictures, (QList<QByteArray>{"first"_ba}));
    }

    void aPictureThatIsNotFollowedByALineEndEndsIt()
    {
        const QByteArray stream = boundary() + "Content-Length: 5\r\n\r\nfirst" + boundary();
        QCOMPARE(readWhole(stream).end, End::WrongBoundary);
    }

    void whatComesBeforeTheFirstBoundaryIsPassedOver()
    {
        // Whatever it is, lines that begin as the boundary does among it: only the boundary
        // itself, where a line begins, is the boundary.
        const QByteArray before = "This is a stream of pictures.\r\n\r\n--\r\n-\r\n--" + word() + "s\r\n--" + word() +
                                  " \r\n--" + word() + "-x\r\n--boundarydonot\r\nnot at the beginning of a line: --" +
                                  word() + "\r\na bare line end\nand a bare \r CR\r\n";
        const Read read = readWhole(before + part("first"_ba) + part("second"_ba) + boundary());
        QCOMPARE(read.end, End::None);
        QCOMPARE(read.pictures, (QList<QByteArray>{"first"_ba, "second"_ba}));

        // As much as the lines of a part may be, and not a byte more.
        const QByteArray just = QByteArray(4094, 'x') + "\r\n";
        QCOMPARE(readWhole(just + part("first"_ba) + boundary()).pictures.size(), 1);
        MultipartPictures reader(word());
        const Read tooMuch = readIn(reader, {"x" + just + part("first"_ba) + boundary()});
        QCOMPARE(tooMuch.end, End::NoBoundary);
        QVERIFY(tooMuch.pictures.isEmpty());
        QCOMPARE(tooMuch.taken, 4097);
        QCOMPARE(reader.room(), 0);
    }

    void aStreamOfAnotherWordIsNoneOfThis()
    {
        // Its parts are what comes before the first boundary, and that has its limit.
        QByteArray stream;
        for (int count = 0; count < 100; ++count) {
            stream += "--anotherword\r\nContent-Type: image/jpeg\r\nContent-Length: 40\r\n\r\n" + picture('p') + "\r\n";
        }
        const Read read = readWhole(stream);
        QCOMPARE(read.end, End::NoBoundary);
        QVERIFY(read.pictures.isEmpty());
        QVERIFY(read.mostRoom <= 8192);
    }

    void theClosingBoundaryEndsTheStream()
    {
        MultipartPictures reader(word());
        const QByteArray closing = "--" + word() + "--";
        const Read read = readIn(reader, {part("last"_ba) + closing + "\r\nwhat comes after it is not read"});
        QCOMPARE(read.end, End::Closed);
        QCOMPARE(read.pictures, (QList<QByteArray>{"last"_ba}));
        QCOMPARE(read.taken, part("last"_ba).size() + closing.size());

        // Also where it is all there is, with or without a line end behind it.
        QCOMPARE(readWhole(closing).end, End::Closed);
        QCOMPARE(readWhole("before it\r\n" + closing + "\r\n").end, End::Closed);
    }

    void whatIsOverTakesNothingMore()
    {
        MultipartPictures reader(word());
        QCOMPARE(readIn(reader, {part("first"_ba) + "--anotherword\r\n"}).end, End::WrongBoundary);
        // Not even a stream that is right.
        const QByteArray right = part("second"_ba) + boundary();
        QCOMPARE(reader.feed(right), 0);
        QVERIFY(!reader.next().has_value());
        QCOMPARE(reader.end(), End::WrongBoundary);
        QCOMPARE(reader.room(), 0);
    }

    void nothingMoreIsTakenWhileAPictureWaits()
    {
        MultipartPictures reader(word());
        const QByteArray first = part("first"_ba);
        const QByteArray stream = first + part("second"_ba) + boundary();
        // Up to the last byte of the picture, and no further: its line end is not read yet.
        const qsizetype taken = reader.feed(stream);
        QCOMPARE(taken, first.size() - 2);
        QCOMPARE(reader.feed(QByteArrayView(stream).sliced(taken)), 0);
        QCOMPARE(reader.next(), std::optional<QByteArray>("first"_ba));
        QVERIFY(!reader.next().has_value());
        const qsizetype more = reader.feed(QByteArrayView(stream).sliced(taken));
        QCOMPARE(taken + more, first.size() + part("second"_ba).size() - 2);
        QCOMPARE(reader.next(), std::optional<QByteArray>("second"_ba));
        QCOMPARE(reader.end(), End::None);
    }

    void aWordThatIsNoneEndsItBeforeItBegan_data()
    {
        QTest::addColumn<QByteArray>("none");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("too long") << QByteArray(71, 'w');
        QTest::newRow("with a line end") << "a\r\nb"_ba;
        QTest::newRow("ending in a blank") << "ab "_ba;
    }

    void aWordThatIsNoneEndsItBeforeItBegan()
    {
        QFETCH(QByteArray, none);
        MultipartPictures reader(none);
        QCOMPARE(reader.end(), End::NoBoundary);
        QCOMPARE(reader.feed("--" + none + "\r\nContent-Length: 1\r\n\r\nx\r\n"), 0);
        QVERIFY(!reader.next().has_value());
    }
};

QTEST_APPLESS_MAIN(TestMultipartPictures)
#include "test_multipart_pictures.moc"
