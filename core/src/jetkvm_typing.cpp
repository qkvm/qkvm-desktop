#include <qkvm/core/jetkvm_typing.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace qkvm::core::jetkvm {

namespace {

constexpr int everyLookMs = 10;
const std::string inUseSentence =
    "Someone is using this JetKVM's own console. It shows its picture to one viewer at a time.";

// Stopped because the JetKVM's own page took the viewer (the web's _taken_while).
base::Error takenWhile(std::size_t done, std::size_t of)
{
    if (done == 0) {
        return {"in_use", "It was not done: " + inUseSentence};
    }
    return {"in_use", "Typing was stopped after " + std::to_string(done) + " of " + std::to_string(of) +
                          " keys: " + inUseSentence};
}

// Stopped because the connection ended under it: nothing more of it went (the web's _ended_while).
base::Error endedWhile(std::size_t done, std::size_t of, const base::Error& why)
{
    if (done == 0) {
        return {"action_failed", "It was not done: " + why.message};
    }
    return {"action_failed",
            "Typing was stopped after " + std::to_string(done) + " of " + std::to_string(of) + " keys: " + why.message};
}

const base::Error noAnswer{"timeout", "The JetKVM did not answer in time"};

JetKvmConnection::Keyboard keyboardOf(const typed::Report& report)
{
    JetKvmConnection::Keyboard keyboard;
    keyboard.modifiers = report.modifiers;
    for (const std::uint8_t key : report.keys) {
        keyboard.keys.append(key);
    }
    return keyboard;
}

void later(int ms, std::function<void()> what)
{
    QTimer::singleShot(ms, QCoreApplication::instance(), std::move(what));
}

struct Typing : std::enable_shared_from_this<Typing> {
    QPointer<JetKvmConnection> on;
    std::vector<typed::Report> reports;
    bool text = false;
    TypingPace pace;
    std::function<void()> keyGoes;
    std::function<void(base::Result<Done>)> done;
    std::size_t at = 0;
    bool over = false;
    // The last letting go sent over hidrpc, by its number, until the JetKVM says it has it.
    quint64 releases = 0;
    bool releasePending = false;

    std::size_t count() const { return reports.size(); }
    int pause() const { return text ? pace.keyPauseMs : pace.holdMs; }

    void finish(const base::Result<Done>& what)
    {
        if (std::exchange(over, true)) {
            return;
        }
        if (on != nullptr) {
            on->typing(false);
        }
        if (done) {
            const std::function<void(base::Result<Done>)> told = std::move(done);
            told(what);
        }
    }
    // Why nothing more goes, where nothing does.
    std::optional<base::Error> stopped(std::size_t typed) const
    {
        if (on == nullptr) {
            return endedWhile(typed, count(), base::Error{"dropped", "The connection to the JetKVM ended"});
        }
        if (on->taken()) {
            return takenWhile(typed, count());
        }
        if (!on->live()) {
            return endedWhile(typed, count(), on->endedError());
        }
        return std::nullopt;
    }

    // What the live line said last goes first (the keys it had down, let go of as it gave the keyboard back): done by
    // the JetKVM in among these, it would land in the text, or let go of a key held for a combination.
    void waitForTheLiveLine(const std::shared_ptr<QElapsedTimer>& clock)
    {
        if (on == nullptr || !on->keysOnTheirWay()) {
            start();
            return;
        }
        if (clock->elapsed() >= pace.liveKeysWaitMs) {
            finish(base::Error{"busy", "Keys from the live view are still on their way to this JetKVM. Try again in a "
                                       "moment."});
            return;
        }
        later(everyLookMs, [self = shared_from_this(), clock] { self->waitForTheLiveLine(clock); });
    }
    void start()
    {
        if (const std::optional<base::Error> why = stopped(0)) {
            finish(*why);
            return;
        }
        on->typing(true);
        next();
    }
    void next()
    {
        if (over) {
            return;
        }
        if (at >= count()) {
            ending(std::make_shared<QElapsedTimer>());
            return;
        }
        if (const std::optional<base::Error> why = stopped(at)) {
            finish(*why);
            return;
        }
        if (keyGoes) {
            keyGoes();
        }
        if (on->road() == JetKvmConnection::Road::Hid) {
            pressOverHid();
        } else {
            pressOverRpc();
        }
    }
    // The last letting go heard of, so that "done" is done; whatever became of it, the text was typed.
    void ending(const std::shared_ptr<QElapsedTimer>& clock)
    {
        if (!clock->isValid()) {
            clock->start();
        }
        if (over) {
            return;
        }
        if (!releasePending || on == nullptr || clock->elapsed() >= pace.answerWithinMs) {
            finish(Done{});
            return;
        }
        later(everyLookMs, [self = shared_from_this(), clock] { self->ending(clock); });
    }

    // --- over hidrpc ---
    void pressOverHid()
    {
        const std::size_t key = at;
        auto settled = std::make_shared<bool>(false);
        const std::shared_ptr<Typing> self = shared_from_this();
        const std::optional<base::Error> failed =
            on->typeWhole(keyboardOf(reports[key]), [self, key, settled](const std::optional<base::Error>& why) {
                if (!std::exchange(*settled, true)) {
                    self->pressedOverHid(key, why);
                }
            });
        if (failed) {
            // Not sent: nothing of this key went. The letting go of the key before it may not have gone either.
            *settled = true;
            on->letGoNow();
            finish(endedWhile(key, count(), *failed));
            return;
        }
        later(pace.answerWithinMs, [self, key, settled] {
            if (!std::exchange(*settled, true)) {
                self->pressedOverHid(key, noAnswer);
            }
        });
    }
    void pressedOverHid(std::size_t key, const std::optional<base::Error>& why)
    {
        if (over) {
            return;
        }
        if (!why) {
            later(pause(), [self = shared_from_this(), key] { self->releaseOverHid(key); });
            return;
        }
        if (why->code == "in_use") {
            // Its own page took the viewer before the JetKVM said it had this press: it may have been done (after the
            // hand-over, where the letting go sent then, behind it, let go of it).
            if (key + 1 == count()) {
                finish(base::Error{"timeout", "The JetKVM's own page took it before it said the last key was pressed: "
                                              "it may have been done"});
                return;
            }
            finish(base::Error{"in_use", "Typing was stopped at key " + std::to_string(key + 1) + " of " +
                                             std::to_string(count()) +
                                             ", which may have been pressed: " + inUseSentence});
            return;
        }
        if (why->code == noAnswer.code && why->message == noAnswer.message && on != nullptr) {
            // No word that it has the press: unknown. Let go of, in turn behind it, and the connection made anew: what
            // comes after it could be done before it or not at all.
            on->letGoNow();
            on->makeAnew(u"a key it was sent was not said done in time"_s);
        }
        // (Or the connection ended under it, cut off: it may have been done; the JetKVM lets go of the keyboard.)
        finish(*why);
    }
    void releaseOverHid(std::size_t key)
    {
        if (over) {
            return;
        }
        const bool last = key + 1 == count();
        if (on == nullptr || on->taken()) {
            // Its own page has the viewer now; the JetKVM let go of this key as it handed it over (and the letting go
            // sent then followed). Nothing more goes.
            finish(last ? base::Result<Done>(Done{}) : base::Result<Done>(takenWhile(key + 1, count())));
            return;
        }
        if (!on->live()) {
            // The JetKVM had this key, and the connection is ending: let go of now, behind it, and nothing more goes.
            const base::Error why = on->endedError();
            on->letGoNow();
            finish(last ? base::Result<Done>(Done{}) : base::Result<Done>(endedWhile(key + 1, count(), why)));
            return;
        }
        const quint64 number = ++releases;
        releasePending = true;
        const std::shared_ptr<Typing> self = shared_from_this();
        const std::optional<base::Error> failed =
            on->typeWhole({}, [self, number](const std::optional<base::Error>& /*why*/) {
                if (number == self->releases) {
                    self->releasePending = false;
                }
            });
        if (failed) {
            releasePending = false;
            on->letGoNow(); // (over the other channel, where this one shut: the JetKVM had the key)
            finish(last ? base::Result<Done>(Done{}) : base::Result<Done>(endedWhile(key + 1, count(), *failed)));
            return;
        }
        at = key + 1;
        later(pace.keyPauseMs, [self] { self->next(); });
    }

    // --- over JSON-RPC ---
    void pressOverRpc()
    {
        const std::size_t key = at;
        const std::shared_ptr<Typing> self = shared_from_this();
        on->press(keyboardOf(reports[key]), pace.answerWithinMs,
                  [self, key](const base::Result<json::Value>& said) { self->pressedOverRpc(key, said); });
    }
    void pressedOverRpc(std::size_t key, const base::Result<json::Value>& said)
    {
        if (over) {
            return;
        }
        if (said) {
            later(pause(), [self = shared_from_this(), key] { self->releaseOverRpc(key); });
            return;
        }
        // The press was not answered: it may still be done, later. Where it went out and no answer came, the
        // connection has let go of it (rule 24); as that release could be done before the press (the JetKVM does each
        // call in a task of its own), the connection is made anew too. (Refused, or never sent, it pressed nothing:
        // made anew all the same.)
        const base::Error& why = said.error();
        if (on != nullptr) {
            on->makeAnew(u"a key it was sent was not answered in time"_s);
        }
        finish(why.code == "dropped" ? endedWhile(key, count(), why) : why);
    }
    void releaseOverRpc(std::size_t key)
    {
        if (over) {
            return;
        }
        const bool last = key + 1 == count();
        if (on == nullptr || on->taken()) {
            // Its own page has the viewer now. This key was let go of: by the JetKVM as it handed the viewer over,
            // where it was done before that; by the connection, where it was done after. A release sent now would land
            // on whoever is at that page: nothing more goes.
            finish(last ? base::Result<Done>(Done{}) : base::Result<Done>(takenWhile(key + 1, count())));
            return;
        }
        const std::shared_ptr<Typing> self = shared_from_this();
        on->press({}, pace.answerWithinMs, [self, key, last](const base::Result<json::Value>& said) {
            if (self->over) {
                return;
            }
            if (!said) {
                if (said.error().code == "dropped") {
                    // The key was pressed, and its letting go could not go: the connection is ending, and the JetKVM
                    // lets go of the keyboard as it ends. Done where it was the last; else stopped after it.
                    self->finish(last ? base::Result<Done>(Done{})
                                      : base::Result<Done>(endedWhile(key + 1, self->count(), said.error())));
                    return;
                }
                self->finish(said.error());
                return;
            }
            self->at = key + 1;
            later(self->pace.keyPauseMs, [self] { self->next(); });
        });
    }
};

} // namespace

void type(QPointer<JetKvmConnection> on, std::vector<typed::Report> reports, bool text, TypingPace pace,
          std::function<void()> keyGoes, std::function<void(base::Result<Done>)> done)
{
    auto typing = std::make_shared<Typing>();
    typing->on = std::move(on);
    typing->reports = std::move(reports);
    typing->text = text;
    typing->pace = pace;
    typing->keyGoes = std::move(keyGoes);
    typing->done = std::move(done);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    typing->waitForTheLiveLine(clock);
}

} // namespace qkvm::core::jetkvm
