#pragma once

#include <optional>

#include <QString>

#include <qkvm/core/server_types.h>
#include <qkvm/engine/types.h>

#include "wall_model.h"

namespace qkvm::ui {

// The card of one machine, from what the server says of it, in the words the page uses for the same
// (app/qkvm/static/app.js: LABELS, veilContent, metaText, fillPanel, fillPorts, pictured). `status`
// is null until the server has said how the machine is. `stream` is the engine's stream for its
// picture, or 0 for a machine that has none; `hasFrame` says that a picture of it has come.
//
// A server, seen through its controller, shows its own screen where a picture of it has come and
// the server runs, as a KVM's card shows a screen; a fault does not cover the screen of a server
// that runs, the card's edge says the rest. Switched off, or with no picture to show, its card says
// how the server is, and names no stream.
//
// The button under what a card says (`action`) is there for a KVM of this PC's own wall, which is
// mended in this program, in the page's words: "Update login", "Review certificate". What a QKVM
// server lists is mended on the server's page, and has none of those. And "Show here", for any machine
// that is in use elsewhere (a JetKVM whose one viewer somebody else has: M6 2h), on this PC's wall or a
// server's, as the page has it; and for a JetKVM of this PC's wall whose video does not come through to
// this PC ("not_through": its cards make no offer after one came to nothing), which says so over its card.
//
// `still`: the last picture that came of it is a still kept between sessions (a PC with Intel AMT whose
// tile is only shown), with its age. The end of the card's line says it, as the page's viewNote does,
// after what it says otherwise (`meta`; and `largeMeta`, for its large view).
struct StillCame {
    std::optional<int> ageSeconds; // how old it is now; nothing: not said
};
WallTile cardFor(const core::Machine& machine, const core::Status* status, quint32 stream, bool hasFrame,
                 const std::optional<StillCame>& still = std::nullopt);

// What a card says of what it shows (the page's viewNote, in its words): for a PC with Intel AMT the
// view it is in ("Health only", "Stills", "Live KVM"), with the age of the still it shows where it
// shows one ("Stills, 2 min ago", "Live KVM · still, 4 s ago"), and in its large view (`large`) that
// it is live there ("Stills on the wall, live here"); for any other machine the age of a still, and
// nothing where it shows none.
QString viewNoteOf(const core::Machine& machine, const core::Status* status, const std::optional<StillCame>& still,
                   bool large);
// One of the three views by the page's word for it (AMT_VIEWS): "Health only", "Stills", "Live KVM".
QString amtViewWord(const QString& view);

// What a machine's card is drawn from (design 4): a JetKVM of this PC's own wall sends its card the newest
// key frame of its H.264, a whole picture by itself (engine::Codec::KeyFrameStill); every other card a JPEG
// or a PNG (a QKVM server makes a JetKVM's into one). Its source says what it sends: how a packet begins
// never decides it.
engine::Codec cardCodecOf(const core::Machine& machine);

// Where the button on a machine's card leads, for a KVM of this PC's own wall: to the box that
// changes it, where the KVM refused the sign-in; and to that box's look at its certificate, where
// it shows another one than it is known by. For any machine that is in use elsewhere ("busy"): to
// taking its picture back, asked first (MachineActions::cardAction); the same for a JetKVM of this PC's
// wall whose video does not come through ("not_through"). Nowhere for any other machine,
// and for one that is neither: such a card has no button.
enum class CardWay { None, UpdateLogin, ReviewCertificate, ShowHere };
CardWay cardWayOf(const core::Machine& machine, const core::Status* status);

// How long ago, as the page writes it: "just now", "45 s ago", "12 min ago", "2 h ago", "3 d ago".
QString ago(int seconds);

// A machine that was told to restart, and what has been seen of that since.
struct Restart {
    qint64 untilMs = 0;    // it is waited for until then, by the clock of whoever keeps this
    bool wentDown = false; // it has been away since it was told
};

// How long a restart is waited for: a KVM is back in about a minute, a controller in one or two.
inline constexpr qint64 restartWithinMs = 180'000;

// The card of a machine that was told to restart, as the page shows one: while the machine is away,
// that is called what it is ("Rebooting…") and not an outage, until the machine is back or clearly
// is not coming. `card` is the machine's card as it would be otherwise, and is changed where the
// machine is away; `restart` is brought up to date. False: the restart is over (the machine came
// back after having been away, or the time is up), the card is left as it is, and `restart` is to
// be forgotten.
bool whileRestarting(WallTile& card, Restart& restart, qint64 nowMs, bool isServer);

// What one of the computers behind a KVM is called: by its name, or by its number where the server
// knows no name for it ("Port 2"). `index` counts from 0.
QString portName(const core::Port& port, qsizetype index);

} // namespace qkvm::ui
