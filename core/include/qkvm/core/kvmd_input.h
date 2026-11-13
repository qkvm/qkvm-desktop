#pragma once

#include <optional>

#include <QByteArray>
#include <QLatin1StringView>
#include <QList>

// What a KVM that runs kvmd (PiKVM, GL.iNet Comet) is told of keyboard and mouse on its line
// (/api/ws): each a short text of JSON, in the words kvmd's own page uses. Nothing here touches
// the network: these are the writers, kept apart from the line they travel on so that every word
// can be tried by itself.
//
// They take what LiveLink's calls of the same names take, so that one screen can drive a QKVM
// server or a KVM. What a KVM has no word for gives nothing, and is never made into another word.
namespace qkvm::core::kvmd {

// The farthest the pointer is over the picture as whoever calls says it (0 to this, each way),
// and as kvmd wants it (from the one to the other, each way).
inline constexpr int farthestAsked = 32767;
inline constexpr int leftmostSaid = -32768;
inline constexpr int rightmostSaid = 32767;
// The most one message moves the pointer, or turns the wheel, each way: kvmd takes no more.
inline constexpr int longestStep = 127;
// The most messages one move is cut into. What a hand moves a mouse between two readings of it
// is far less; a number from nowhere does not become a million messages.
inline constexpr int mostSteps = 32;

// The name kvmd knows a key by: the web's name for the place of a key on the keyboard
// (KeyboardEvent.code, "KeyA", "ShiftLeft"), from the key's USB number (HID usage page 7, as
// input/keys gives it). Nothing for a number that is no key, for a key kvmd is not known to have a
// name for, and for the keyboard's Power key (0x66), which is never said to a machine.
std::optional<QLatin1StringView> keyName(int usage);

// A key went down or up. Nothing for a key without a name.
std::optional<QByteArray> keyWord(int usage, bool down);
// A button of the mouse went down or up: 1 left, 2 right, 3 middle, 4 back, 5 forward. Nothing
// for any other number.
std::optional<QByteArray> buttonWord(int number, bool down);
// The pointer is at this place, each way 0 to 32767 over the picture; what is beyond is the edge.
// (For a KVM whose mouse says where it is. One whose mouse says how far it moved takes
// pointerByWords, and which of the two a KVM has is its own to say.)
QByteArray pointerAtWord(int x, int y);
// The pointer moved by so much: one message for a short move, several for a long one, which
// together are that move and go along a straight line; none for no move.
QList<QByteArray> pointerByWords(int dx, int dy);
// The wheel turned by so much, each way cut to what one message takes; nothing for no turn.
std::optional<QByteArray> wheelWord(int dx, int dy);
// "Are you there", as kvmd's page asks it. (A text in either wording.)
QByteArray pingWord();

// The same, in the other wording kvmd has: short messages of bytes, which are what its own pages
// send, and all that one maker's KVM was seen to take (a real one, 2026-10-12: what was said to it
// as text did nothing). The first byte says what it is (1 a key, 2 a button, 3 where the pointer
// is, 4 how far it moved, 5 the wheel). For a key and a button the second byte is 1 for down and
// 0 for up, and the name follows in ASCII; a place is two numbers of sixteen bits, the high byte
// first; a move and a turn are a zero and two numbers of eight bits. They are sent as messages of
// bytes, not as texts (DeviceLine::sayBytes). What has no word in the one wording has none in the
// other.
std::optional<QByteArray> keyBytes(int usage, bool down);
std::optional<QByteArray> buttonBytes(int number, bool down);
QByteArray pointerAtBytes(int x, int y);
QList<QByteArray> pointerByBytes(int dx, int dy);
std::optional<QByteArray> wheelBytes(int dx, int dy);

} // namespace qkvm::core::kvmd
