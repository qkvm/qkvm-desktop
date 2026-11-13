#pragma once

#include <optional>

#include <QByteArray>
#include <QByteArrayView>

#include <qkvm/core/kvmd_answers.h>

// A KVM's live picture as its stream of JPEGs, read (SPEC.md section 6.3): an answer of the kind
// "multipart/x-mixed-replace", which carries one picture after the other and never ends, as
// ustreamer sends it and kvmd passes it on. Every part of it is
//
//     --<word> CR LF
//     header lines, each ended by CR LF ("Content-Length: N" is the one that is read)
//     CR LF
//     N bytes
//     CR LF
//
// Nothing here touches the network: this is the reader, kept apart from the line the stream
// travels on so that it can be tried with whatever a device might send. A KVM is somebody else's
// software on the network: the stream is read strictly, every size in it has a limit, and what
// breaks a rule ends the reading for good.
namespace qkvm::core {

// The word between the parts, as an answer's Content-Type names it. Nothing where the kind is not
// multipart/x-mixed-replace, or where it does not name, once, a word that RFC 2046 allows: 1 to
// 70 of its characters, the last of them no blank, in quotes or without.
std::optional<QByteArray> boundaryFrom(QByteArrayView contentType);

// Fed the bytes of such a stream as they arrive, in whatever cut, it hands out the pictures.
//
// What it hands out are a part's N bytes as they came. Whether they are a picture is not looked
// at here, neither by what the part calls itself nor by how it begins: that is the decoder's to
// say, and whoever shows them has to ask it.
//
// It keeps one part at most: the lines a part begins with, and the picture under way or the one
// that is whole and has not been taken yet. The length that a part states is not what room is
// made for: a first piece of a few hundred kilobytes, which most pictures fit in, then as much
// again each time the bytes that came need it, and never more than was stated.
class MultipartPictures {
public:
    // Why the reading is over.
    enum class End {
        None,           // it is not
        Closed,         // the KVM said so: the closing "--<word>--"
        NoBoundary,     // more came before the first boundary than is passed over; or the word is none
        WrongBoundary,  // what follows a picture is not the boundary
        BadLineEnd,     // a line of the stream's own ends in LF alone, or in CR without LF
        HeadersTooLong, // the lines a part begins with are more than is read
        NoLength,       // a part does not say how long it is
        BadLength,      // it says so in what is no length: no number, a sign, 0, or twice
        TooLarge,       // it says more than is taken as one picture
    };

    struct Limits {
        // The lines a part begins with, the empty one that ends them included; and what may come
        // before the first boundary.
        qsizetype headerBytes = 4096;
        qsizetype pictureBytes = kvmd::largestPictureBytes;
    };

    // `boundary`: the word, as boundaryFrom gave it.
    explicit MultipartPictures(const QByteArray& boundary);
    MultipartPictures(const QByteArray& boundary, Limits limits);

    // Takes bytes of the stream and says how many of them it took. That is all of them, unless a
    // picture became whole (nothing more is taken until next() has handed that one out), or the
    // reading is over (nothing is taken ever again). So whoever feeds it goes on with the rest
    // after next(), and stops when end() says why.
    qsizetype feed(QByteArrayView bytes);
    // The picture that became whole, once; nothing while none has.
    std::optional<QByteArray> next();

    End end() const { return m_end; }
    // How much memory it holds for the part under way and the picture not yet taken. (For the
    // tests, which look that a stated length takes none.)
    qsizetype room() const;

private:
    enum class At {
        LineStart, // before the first boundary, where a line begins
        Passing,   // before the first boundary, in a line that is not it
        Tail,      // behind "--<word>": the end of that line, or the two dashes that close the stream
        Headers,
        Body,
        AfterBody, // the line end behind a picture, and "--<word>" after it
    };

    void step(char byte);
    qsizetype body(QByteArrayView bytes);
    void passedOver(qsizetype bytes);
    void headerLine();
    void over(End why);

    QByteArray m_dashes; // "--<word>"
    Limits m_limits;
    End m_end = End::None;
    At m_at = At::LineStart;
    bool m_first = true;    // no boundary has been found yet
    qsizetype m_passed = 0; // bytes before the first boundary
    qsizetype m_matched = 0;
    int m_tail = 0;    // behind the word: 0 nothing yet, 1 a CR, 2 a dash
    bool m_cr = false; // the byte before this one was a CR
    qsizetype m_headerBytes = 0;
    QByteArray m_line;
    qsizetype m_stated = -1; // the length this part states; -1: none yet
    qsizetype m_left = 0;    // of the picture under way
    QByteArray m_picture;
    std::optional<QByteArray> m_ready;
};

} // namespace qkvm::core
