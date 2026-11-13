#pragma once

#include <QByteArray>
#include <QList>
#include <QUrl>
#include <optional>

namespace qkvm::core {

// The one H.264 video of a session description that this program decodes: payload type, mid, format
// line, and the SPS and PPS where the description carries them. Moved here from webrtc_receiver.cpp
// as it was (M6 2b).
struct VideoChoice {
    int payload = -1;
    QByteArray mid;
    QByteArray format;
    QByteArray sps;
    QByteArray pps;
};

// Nothing where the description is no session description this program takes: larger than 64 KiB, a
// NUL in it, no SHA-256 fingerprint, other than one video, a video line that is off or of another
// transport, a video that only receives or does nothing, a mid that is no short token, or no H.264 of
// packetization mode 1 in a profile the decoder takes.
// `onlyThese` empty: any payload, the first H.264 in the order the description maps them (a Janus
// offer). Otherwise only one of those (an answer to an offer of ours): the first of them in the video
// line's own order (M6 2e).
std::optional<VideoChoice> chooseVideo(const QByteArray& sdp, const QList<int>& onlyThese = {});

// Whether the description has one application (data channels), whose port is not 0 (M6 2e).
bool hasApplication(const QByteArray& sdp);
// The mid of its one application, or nothing (2e's reading: an answer's is held to the offer's).
std::optional<QByteArray> applicationMid(const QByteArray& sdp);

// A description with its own addresses taken out of it: each `a=candidate:` line, as "candidate:..." in
// the order they stood, and `a=end-of-candidates`; the rest as it stood. So that an address inside an
// answer goes the one way every address of the far end's goes, judged and counted (2e's reading: the
// library took those straight, beyond any count, and looked up the names among them).
struct WithoutAddresses {
    QByteArray rest;
    QList<QByteArray> candidates;
};
WithoutAddresses withoutAddresses(const QByteArray& sdp);

// What is wrong with an address of the far end's for this program (2e's reading), and where nothing is,
// the address as the library is to be handed it: made anew of the fields judged, never the far end's own
// text (2e, the second reading: the library reads a candidate by whitespace of any kind, so that a tab
// could have it read another host than the one judged here).
//
// Nothing is wrong where it is "candidate:" and the words ICE says, each byte a visible one of ASCII or
// a single space between two words; its foundation 1 to 32 letters, digits, "+" or "/"; its component,
// priority and port numbers (1 to 256, up to 2^32 - 1, 1 to 65535); its type host, srflx, prflx or
// relay; its transport UDP; its host a number (an IPv4 address in four parts, or an IPv6 one: a name is
// never looked up, a local one of mDNS among them); and one that may be reached: not unspecified (nor
// in 0.0.0.0/8, nor IPv4 written in the old way inside ::/96), multicast or the broadcast, and not this
// PC's loopback unless that is allowed (the far end's own signalling address is loopback). What follows
// its type is not handed on.
enum class AddressFault { None, Malformed, NotUdp, Named, Unreachable };
struct JudgedAddress {
    AddressFault fault = AddressFault::Malformed;
    // Where nothing is wrong: "candidate:<foundation> <component> UDP <priority> <host> <port> typ <type>",
    // the host as the number judged (an IPv4 address written as IPv6 as the IPv4 one it is).
    QByteArray candidate;
};
JudgedAddress judgeAddress(const QByteArray& candidate, bool loopbackAllowed);

// Whether a device is spoken to at this PC's own loopback ("localhost", or a loopback address by its number):
// only then are the loopback addresses it says taken (a test's stand-in; a JetKVM's and a Comet's alike).
bool spokenToOnThisPc(const QUrl& address);

} // namespace qkvm::core
