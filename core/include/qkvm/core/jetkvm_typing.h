#pragma once

#include <functional>
#include <vector>

#include <QPointer>

#include <qkvm/base/result.h>
#include <qkvm/core/jetkvm_connection.h>
#include <qkvm/core/server_types.h>
#include <qkvm/core/typed_keys.h>

// An action that types on a JetKVM (keys pressed together, a text), over the connection its keys are ready on, as
// the web's JetKvmDevice.act does it (jetkvm.py, _type_over_hid and its JSON-RPC loop; rule 24 of its
// FOR-THE-DESKTOP-APP, part 3):
// * It waits up to liveKeysWaitMs for the live line's reports still on their way over JSON-RPC (done in among these,
//   one would land in the text, or let go of a key held for a combination), then answers `busy`.
// * Over hidrpc, each key: the whole keyboard with it (`02`), the JetKVM's word that it has it (`33`, the answer:
//   answerWithinMs), a pause (keyPauseMs in a text, holdMs for keys pressed together), the whole keyboard with
//   nothing down, a pause. Over JSON-RPC the same as two keyboardReport calls, each once the one before was answered.
//   One key on its way at most; nothing lets go of a key by itself.
// * It stops with `in_use` where the JetKVM's own page takes it, except after its last key was pressed (then it is
//   done); where a key it pressed is not answered, that key is let go of and the connection is made anew (the JetKVM
//   could do the release before the press), and it answers `timeout` ("may have been done"). Where the connection
//   ends under it, nothing more goes, and it says after how many keys.
// On the interface thread.
namespace qkvm::core::jetkvm {

struct TypingPace {
    int answerWithinMs = 8'000; // the web's RPC_TIMEOUT
    int keyPauseMs = 4;         // KEY_PAUSE
    int holdMs = 50;            // HOLD
    int liveKeysWaitMs = 5'000; // LIVE_KEYS_WAIT
};

// `text`: the reports type a text (paced so), not keys pressed together. `keyGoes` is said before every key: the
// connection is in use all that while, whoever looks or does not (the web's _wanted_at before every key: a long text
// left to count as unwatched was cut in the middle; review 3's LOW 3).
void type(QPointer<JetKvmConnection> on, std::vector<typed::Report> reports, bool text, TypingPace pace,
          std::function<void()> keyGoes, std::function<void(base::Result<Done>)> done);

} // namespace qkvm::core::jetkvm
