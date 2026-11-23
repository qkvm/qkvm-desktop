#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QNetworkCookie>
#include <QString>

#include <optional>

#include <qkvm/core/strict_json.h>

// What a JetKVM says, read as its connection (jetkvm_connection.cpp, jetkvm_keys.cpp) and its driver
// (jetkvm_device.cpp) read it, each reading pure: bytes in, what was read out, which they then act on. Apart from them
// so that what a JetKVM may send can be fuzzed and tried without a connection or a line (core/fuzz/fuzz_jetkvm.h,
// test_core_fuzz_found): moved here as it was, the acting left where it stood.
namespace qkvm::core::jetkvm_words {

// --- on its signalling line (JetKvmConnection::read) ----------------------------------------------------------------

struct Signalling {
    enum class Kind {
        Nothing, // no JSON object (a "pong"), or not for this connection: let go
        Refused, // {"error": ...} before its answer, in place of one
        Answer,  // its answer to the offer
        Address, // one of its addresses (`new-ice-candidate`)
    };
    Kind kind = Kind::Nothing;
    QString refusal;    // Refused: its words, one short line (empty: it gave none)
    json::Value data;   // Answer: its `data`, as it came
    QByteArray address; // Address: its candidate, as it said it
};
// `answered`: its answer came already (neither a refusal nor an answer is read after it).
Signalling signalling(QByteArrayView text, bool answered);
// Its answer's description, from the `data` its answer came with: base64 of JSON whose `sdp` is text, not empty;
// nothing where it is not that (`bad_response`).
std::optional<QByteArray> answerSdp(const json::Value& data);

// --- on `rpc` (JetKvmConnection::heard)
// ---------------------------------------------------------------------------------

struct Rpc {
    enum class Kind {
        Nothing,      // no JSON object, an answer under a number that is none, or what is let go
        Answer,       // the answer to a call of a number (`id`)
        OtherSession, // otherSessionConnected
        Video,        // videoInputState
        Usb,          // usbState
        Atx,          // atxState
        Version,      // localVersion
        Keys,         // keysDownState (the connection takes it only on hidrpc's road)
    };
    Kind kind = Kind::Nothing;
    qint64 id = 0;             // Answer: the number it answers, a whole one from 1
    bool refused = false;      // Answer: it said an `error` that is not null
    QString words;             // Answer refused: its words made plain (empty: none); Usb, Version: the text
    json::Value result;        // Answer not refused: its `result` (null where none); Atx: its `params`
    std::optional<bool> ready; // Video: whether there is a picture, where said as a flag
    int width = 0;             // Video: clamped
    int height = 0;
    double fps = 0;
    int modifiers = 0; // Keys: the modifiers as bits, and the other keys down
    QList<int> keys;
};
Rpc rpc(QByteArrayView bytes);
// Whether a text it said on `rpc` is the word that another viewer took it: read on the library's thread, so pure
// (json::parse is).
bool otherSessionConnected(QByteArrayView text);

// --- on `hidrpc` (JetKvmConnection::heardHid)
// -------------------------------------------------------------------------

// The version of hidrpc this program speaks (its handshake, `01 01`).
inline constexpr char hidVersion = 1;

struct Hid {
    enum class Kind {
        Nothing,
        Shaken,       // `01 01`: it speaks hidrpc of this program's version
        OtherVersion, // `01 v`: of another
        Keys,         // `33 m k1..k6`: its keyboard as it is (the connection takes it only on hidrpc's road)
    };
    Kind kind = Kind::Nothing;
    int modifiers = 0;
    QList<int> keys;
};
Hid hid(QByteArrayView bytes);

// --- to its driver (JetKvmDevice)
// -------------------------------------------------------------------------------------

// The pass a sign-in handed over: its cookie, where it is one (the web's TOKEN, kvmd::isToken).
QByteArray passFrom(const QList<QNetworkCookie>& cookies);
// A 429's Retry-After, as a wait: a number of seconds, or none. As much as `atMostMs` where it says more, "inf" among
// it.
qint64 comeBackAfterMs(const QByteArray& said, qint64 atMostMs);
// Its own number, as a mark to know it by at any address: only ever compared with another, and so kept as a digest (the
// number itself is nobody's business). None where what it gave is no number fit to be one.
QString numberFrom(const QByteArray& answer);
// Whether GET /device/status says it is fresh from its box: set up by nobody yet (`isSetup` plainly false).
bool saysNotSetUp(const QByteArray& status);
// The version a line's `device-metadata` word says, one short line; nothing where the word is another.
std::optional<QString> metadataVersion(const QByteArray& text);

} // namespace qkvm::core::jetkvm_words
