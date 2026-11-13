#include <qkvm/core/kvmd_console_line.h>

#include <algorithm>
#include <optional>
#include <utility>

#include <QCoreApplication>

#include <qkvm/core/kvmd_input.h>
#include <qkvm/core/tls.h>

namespace qkvm::core {

namespace {

DeviceLine::Where lineTo(KvmdConsoleLine::Where where)
{
    DeviceLine::Where line;
    line.address = std::move(where.address);
    line.asking = KvmdConsoleLine::path;
    if (!where.query.isEmpty()) {
        line.asking += '?' + where.query;
    }
    line.cookie = std::move(where.cookie);
    line.knownByCertificate = where.knownByCertificate;
    line.certificate = std::move(where.certificate);
    return line;
}

DeviceLine::Limits heldTo(const KvmdConsoleLine::Pace& pace)
{
    DeviceLine::Limits limits;
    limits.letInWithinMs = pace.letInWithinMs;
    limits.mostUnsentBytes = pace.mostUnsentBytes;
    limits.goodbyeWithinMs = pace.goodbyeWithinMs;
    limits.lateAfterMs = pace.lateAfterMs;
    return limits;
}

// Why a line ended, in the driver's codes and words (kvmd_device.cpp): whoever shows the one
// shows the other.
struct Said {
    const char* code;
    const char* message;
};

Said saidOf(DeviceLine::End why)
{
    switch (why) {
    case DeviceLine::End::NotAsked:
        return {"bad_response", "What was to be asked of the device for keyboard and mouse is not fit to be sent"};
    case DeviceLine::End::TlsDamaged:
        return {tlsDamaged, tlsDamagedSentence};
    case DeviceLine::End::NotReached:
        return {"unreachable", "Device is unreachable"};
    case DeviceLine::End::CertificateUnread:
        return {"cert_invalid",
                "The certificate saved for this device cannot be read. Look at its certificate and trust it again."};
    case DeviceLine::End::CertificateChanged:
        return {"cert_mismatch", "Certificate does not match the one saved for this device"};
    case DeviceLine::End::CertificateUntrusted:
        return {"cert_invalid", "The device's certificate is not trusted"};
    case DeviceLine::End::CertificateOfUnknownSigner:
        return {"cert_invalid", "The device's certificate was signed by a maker of certificates that Windows does not "
                                "know. Have Windows trust that maker, and the device is let in by that."};
    case DeviceLine::End::Silent:
        return {"timeout", "Device did not respond in time"};
    case DeviceLine::End::SignedOut:
        return {"auth_failed", "The device did not accept the session for keyboard and mouse"};
    case DeviceLine::End::NotLetIn:
        return {"bad_response", "The device did not open a line for keyboard and mouse"};
    case DeviceLine::End::Fell:
        return {"dropped", "The device closed the connection"};
    case DeviceLine::End::Misbehaved:
        return {"dropped", "The device sent what this program does not accept"};
    case DeviceLine::End::TakesNothing:
        return {"dropped", "The device stopped taking keyboard and mouse"};
    case DeviceLine::End::Late:
        return {"timeout", "The device did not take keyboard and mouse in time"};
    }
    return {"dropped", "The device closed the connection"};
}

// What a cookie carries behind its "=": the pass. Nothing where it is no pair.
QByteArray passIn(const QByteArray& cookie)
{
    const qsizetype sign = cookie.indexOf('=');
    return sign < 0 ? QByteArray() : cookie.sliced(sign + 1);
}

// Whether one of the eight keys that change what another key means is among these: Ctrl, Shift,
// Alt and the Windows key, left and right, by USB's numbers for them.
bool holdAModifier(const QList<int>& keys)
{
    constexpr int first = 0xE0;
    constexpr int last = 0xE7;
    return std::ranges::any_of(keys, [](int usage) { return usage >= first && usage <= last; });
}

} // namespace

KvmdConsoleLine::KvmdConsoleLine(Where where, QObject* parent) : KvmdConsoleLine(std::move(where), Pace{}, parent) {}

KvmdConsoleLine::KvmdConsoleLine(Where where, Pace pace, QObject* parent)
    : QObject(parent), m_inTexts(where.inTexts), m_pass(passIn(where.cookie)),
      m_line(lineTo(std::move(where)), heldTo(pace))
{
    connect(&m_line, &DeviceLine::letIn, this, [this] {
        m_open = true;
        m_asking.start();
        m_silence.start();
        emit opened();
    });
    // Whatever it was, it came: the KVM is there, and what was said to it before is taken to
    // have got there.
    connect(&m_line, &DeviceLine::heard, this, [this] {
        if (m_open) {
            m_silence.start();
            m_saidUpUnheard = false;
        }
    });
    connect(&m_line, &DeviceLine::ended, this, [this](DeviceLine::End why) {
        const Said said = saidOf(why);
        gone(said.code, said.message);
    });
    m_asking.setInterval(pace.askEveryMs);
    connect(&m_asking, &QTimer::timeout, this, [this] { say(kvmd::pingWord()); });
    // A line that stalls fails nothing, and says nothing by itself. So the KVM is asked now and
    // then whether it is there, and one of which nothing at all has come for too long is gone:
    // cut without a goodbye, so that nothing which still waits to go to it goes later.
    m_silence.setSingleShot(true);
    m_silence.setInterval(pace.silentAfterMs);
    connect(&m_silence, &QTimer::timeout, this, [this] {
        m_line.cut();
        gone("timeout", "Device did not respond in time");
    });
    // Begun when whoever made this can hear what it says.
    QTimer::singleShot(0, this, [this] {
        if (!m_ended) {
            m_line.open();
        }
    });
}

KvmdConsoleLine::~KvmdConsoleLine()
{
    // Thrown away while keys are down: what can still be said is said, and goodbye, and both are
    // pushed out. Then the line goes with this, in the same breath: neither waited for nor reset
    // (DeviceLine::close says what that leaves open). Nobody is told of an end from in here, nor
    // of what could not be let go: whoever is done with a line and goes on hands it over instead
    // (letGo), as KvmdLive does with every line it is done with.
    disconnect(&m_line, nullptr, this, nullptr);
    if (!m_ended) {
        releaseAll();
        static_cast<void>(m_line.close());
    }
}

bool KvmdConsoleLine::say(const QByteArray& word)
{
    // (A line that takes no more, or has made what was said wait too long, ends in there, and
    // is not open after it.)
    return m_open && m_line.say(word);
}

bool KvmdConsoleLine::sayOfInput(const QByteArray& word)
{
    if (!m_open) {
        return false;
    }
    return m_inTexts ? m_line.say(word) : m_line.sayBytes(word);
}

void KvmdConsoleLine::over()
{
    m_open = false;
    m_ended = true;
    m_asking.stop();
    m_silence.stop();
    m_keysDown.clear();
    m_buttonsDown.clear();
    m_keysHeld.clear();
    m_buttonsHeld.clear();
    m_saidUpUnheard = false;
}

void KvmdConsoleLine::gone(const char* code, const char* message)
{
    // What is down on the machine cannot be let go from here any more. It is forgotten here, and
    // whoever hears of the end hears that there was something. So it is with what was said to
    // be up since the KVM was last heard from: Windows took that word, the line ended, and what
    // Windows still held went with it. On the machine it may be down still.
    const bool leftDown = !m_keysDown.isEmpty() || !m_buttonsDown.isEmpty() || m_saidUpUnheard;
    over();
    emit closed(QString::fromLatin1(code), QString::fromLatin1(message), leftDown);
}

void KvmdConsoleLine::key(int usage, bool down)
{
    const std::optional<QByteArray> word = m_inTexts ? kvmd::keyWord(usage, down) : kvmd::keyBytes(usage, down);
    if (m_ended || !word) {
        return;
    }
    if (!down) {
        // Up. Where it was only held here, that is over, and nothing is said: it never went
        // down on the machine. Where it is down there, it stays noted as that until the machine
        // was told, so that a line which ends over the telling says that it left something down.
        m_keysHeld.removeOne(usage);
        if (m_keysDown.contains(usage) && sayOfInput(*word)) {
            m_keysDown.removeOne(usage);
            m_saidUpUnheard = true;
        }
        return;
    }
    // Held: said once where it is down on the machine, and not at all where it went down while
    // the line could not say so. That one is no press when Windows repeats it, either.
    if (m_keysDown.contains(usage) || m_keysHeld.contains(usage)) {
        return;
    }
    // Nor can the line say it under a modifier that is held here and up on the machine: Ctrl
    // still held when everything was let go, and C pressed again, would be a bare "c" there.
    // Such a key is held as that modifier is, until it was let go here.
    if (!m_open || holdAModifier(m_keysHeld)) {
        m_keysHeld.append(usage);
    } else if (sayOfInput(*word)) {
        m_keysDown.append(usage);
    }
}

void KvmdConsoleLine::button(int number, bool down)
{
    const std::optional<QByteArray> word = m_inTexts ? kvmd::buttonWord(number, down) : kvmd::buttonBytes(number, down);
    if (m_ended || !word) {
        return;
    }
    if (!down) {
        m_buttonsHeld.removeOne(number);
        if (m_buttonsDown.contains(number) && sayOfInput(*word)) {
            m_buttonsDown.removeOne(number);
            m_saidUpUnheard = true;
        }
        return;
    }
    if (m_buttonsDown.contains(number) || m_buttonsHeld.contains(number)) {
        return;
    }
    // Nor under a modifier that is held here and up on the machine, as for a key: Ctrl and a
    // click would arrive as a bare click there. Such a button is held as that modifier is, until
    // it was let go here.
    if (!m_open || holdAModifier(m_keysHeld)) {
        m_buttonsHeld.append(number);
    } else if (sayOfInput(*word)) {
        m_buttonsDown.append(number);
    }
}

void KvmdConsoleLine::pointerAt(int x, int y)
{
    if (m_open) {
        sayOfInput(m_inTexts ? kvmd::pointerAtWord(x, y) : kvmd::pointerAtBytes(x, y));
    }
}

void KvmdConsoleLine::pointerBy(int dx, int dy)
{
    if (!m_open) {
        return;
    }
    const QList<QByteArray> words = m_inTexts ? kvmd::pointerByWords(dx, dy) : kvmd::pointerByBytes(dx, dy);
    for (const QByteArray& word : words) {
        if (!sayOfInput(word)) {
            return;
        }
    }
}

void KvmdConsoleLine::wheel(int dx, int dy)
{
    if (const std::optional<QByteArray> word = m_inTexts ? kvmd::wheelWord(dx, dy) : kvmd::wheelBytes(dx, dy)) {
        sayOfInput(*word);
    }
}

void KvmdConsoleLine::releaseAll()
{
    // The buttons, then the keys; of each the last that went down first. Each stays noted as
    // down until the machine was told. A line that ends over the telling has forgotten them all by
    // then (or is being closed, and will): nothing more is said.
    const QList<int> buttons = m_buttonsDown;
    for (auto number = buttons.crbegin(); number != buttons.crend(); ++number) {
        const std::optional<QByteArray> word =
            m_inTexts ? kvmd::buttonWord(*number, false) : kvmd::buttonBytes(*number, false);
        if (word && !sayOfInput(*word)) {
            return;
        }
        m_buttonsDown.removeOne(*number);
        m_saidUpUnheard = true;
        // Let go on the machine, and whether a finger is still on it nobody has said: held here
        // until it is told to be up, and no press until then.
        m_buttonsHeld.append(*number);
    }
    const QList<int> keys = m_keysDown;
    for (auto usage = keys.crbegin(); usage != keys.crend(); ++usage) {
        const std::optional<QByteArray> word = m_inTexts ? kvmd::keyWord(*usage, false) : kvmd::keyBytes(*usage, false);
        if (word && !sayOfInput(*word)) {
            return;
        }
        m_keysDown.removeOne(*usage);
        m_saidUpUnheard = true;
        m_keysHeld.append(*usage);
    }
}

bool KvmdConsoleLine::close()
{
    if (m_ended) {
        return true;
    }
    // From here on nothing the line says of itself is heard: it is ended from here, whatever
    // comes of letting go, and whoever closes it is not told from in here that it ended by
    // itself. (A line that takes no more of the letting go is cut in there, and what could not be
    // said stays down on the machine.)
    disconnect(&m_line, nullptr, this, nullptr);
    // What is down on the machine is let go while the line can still say so.
    releaseAll();
    // What the letting go could not say is still noted as down. And what was said to be up
    // since the KVM was last heard from, by the letting go or before it, has to be pushed out
    // with the goodbye: with a line that is reset in there instead it goes, as where a line
    // ends by itself (gone()).
    const bool allSaidUp = m_keysDown.isEmpty() && m_buttonsDown.isEmpty();
    const bool saidUpUnheard = m_saidUpUnheard;
    over();
    const bool pushedOut = m_line.close();
    // Where an "up" was said that the KVM was not heard from after, "all let go" rests on what
    // was only offered to Windows: whether it arrived shows when the line has gone (letGo).
    m_restsOnAnUp = allSaidUp && pushedOut && saidUpUnheard;
    return allSaidUp && (pushedOut || !saidUpUnheard);
}

bool KvmdConsoleLine::letGo(std::unique_ptr<KvmdConsoleLine> line, std::function<void()> stillDown,
                            std::function<void()> gone)
{
    // (What has nothing to wait for is told a turn later all the same: never from inside this call.)
    const auto goneLater = [&gone] {
        if (gone) {
            QTimer::singleShot(0, QCoreApplication::instance(), std::move(gone));
        }
    };
    if (line == nullptr) {
        goneLater();
        return true;
    }
    KvmdConsoleLine* const left = line.get();
    if (left->m_ended) {
        // It ended by itself before this, and was reset then: there is nothing to wait for.
        line.release()->deleteLater();
        goneLater();
        return true;
    }
    const bool allLetGo = left->close();
    // (After the closing, which lets go of everything the line was heard by. `gone` is never said
    // from inside close(), and is said also of a line that the letting go itself has ended.)
    // The line has gone: thrown away. And where the closing said that all was let go, on the
    // strength of an "up" that the KVM is not known to have got, and the line went without the
    // KVM's goodbye (it fell, or its time was up and it was reset with the "up" in it): whoever
    // held it is told first that something may still be down after all. Late, and still told.
    connect(&left->m_line, &DeviceLine::gone, left,
            [left, mayBeDown = left->m_restsOnAnUp, stillDown = std::move(stillDown),
             gone = std::move(gone)](bool arrived) {
                if (!arrived && mayBeDown && stillDown) {
                    stillDown();
                }
                if (gone) {
                    gone();
                }
                left->deleteLater();
            });
    line.release()->setParent(QCoreApplication::instance());
    return allLetGo;
}

} // namespace qkvm::core
