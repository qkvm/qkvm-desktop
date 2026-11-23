#include "rfb_reader.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
#include <string>
#include <utility>
// Built with the standard library's own checks in every build, as screen_reader.cpp is (core/CMakeLists.txt).
#if !_MSVC_STL_HARDENING_ARRAY || !_MSVC_STL_HARDENING_SPAN || !_MSVC_STL_HARDENING_VECTOR
#error "This holds what a VNC server's screen came to, and is built with _MSVC_STL_HARDENING"
#endif

#include <QThreadPool>

#include <qkvm/base/screen_still.h>
#include <qkvm/core/vnc_auth.h>

// Windows' own header, for the running thread's processor time (GetThreadTimes).
#include <windows.h>

namespace qkvm::core {

namespace aspeed = bmc::aspeed;

namespace {

// Whether what an update changed is most of the screen: then it goes over whole.
bool mostOf(std::span<const rfb::Rect> changed, int width, int height)
{
    std::uint64_t area = 0;
    for (const rfb::Rect& rect : changed) {
        area += static_cast<std::uint64_t>(rect.width) * static_cast<std::uint64_t>(rect.height);
    }
    const std::uint64_t all = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    return all > 0 && 2 * area >= all;
}

// A rectangle of a screen `width` by `height` copied from `from` into `into` (both of that size).
void copy(std::span<const std::uint8_t> from, std::span<std::uint8_t> into, int width, const aspeed::Rect& rect)
{
    const std::size_t across = std::size_t{3} * static_cast<std::size_t>(rect.width);
    const std::size_t row = std::size_t{3} * static_cast<std::size_t>(width);
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        const std::size_t at = static_cast<std::size_t>(y) * row + std::size_t{3} * static_cast<std::size_t>(rect.x);
        std::memcpy(into.subspan(at, across).data(), from.subspan(at, across).data(), across);
    }
}

// A text cut to `most` UTF-16 units before a character that takes two, and never through it.
QString wholeLeft(QString text, qsizetype most)
{
    if (text.size() > most) {
        text.truncate(text.at(most - 1).isHighSurrogate() ? most - 1 : most);
    }
    return text;
}

// The name a server gives itself, as one short line of what can be shown of it: each character whole (one beyond the
// first plane is two UTF-16 units, and is kept where it can be shown, as the web's kvm.plain keeps what Python finds
// printable), any other a blank, and cut at 80 before a character and never through one (outside review c9a-2, L5).
QString plainName(const std::string& said)
{
    const std::u32string read = QString::fromUtf8(said.data(), static_cast<qsizetype>(said.size())).toStdU32String();
    QString name;
    name.reserve(static_cast<qsizetype>(read.size()));
    for (const char32_t one : read) {
        if (QChar::isPrint(one)) {
            name.append(QString::fromUcs4(&one, 1));
        } else {
            name.append(u' ');
        }
    }
    return wholeLeft(name.simplified(), 80);
}

// The running thread's own processor time in milliseconds (how long it has actually worked on a core), for the
// drawing share: a reader kept off a core by other work is not charged the wall it lost. Windows counts it at each tick
// of its clock (about 15.6 ms), so a step of a millisecond or two may read nought, which only makes the share kinder.
std::int64_t threadWorkMs()
{
    FILETIME created{};
    FILETIME ended{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &ended, &kernel, &user) == 0) {
        return RfbReader::steadyMs();
    }
    const auto hundredNs = [](const FILETIME& time) {
        return (static_cast<std::int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return (hundredNs(kernel) + hundredNs(user)) / 10000; // 100 ns units to milliseconds
}

// The clocks the share is kept by, set on the reader's own thread when it runs (GetCurrentThread then): a test's own
// clocks, where it gave any, are left as they are. And what says that the reader was detached, which the read asks as
// often as it asks the share (the reader's own: it lives as long as the client).
rfb::Options withClocks(rfb::Options options, const std::atomic<bool>* stopping)
{
    if (!options.clock) {
        options.clock = [] { return RfbReader::steadyMs(); };
    }
    if (!options.workClock) {
        options.workClock = [] { return threadWorkMs(); };
    }
    options.stopped = [stopping] { return stopping->load(std::memory_order_relaxed); };
    return options;
}

} // namespace

RfbReader::RfbReader(QObject* home, std::function<void()> ready, rfb::Options options, SecretText password)
    : m_client(withClocks(std::move(options), &m_stopping)), m_password(std::move(password)),
      m_holdsAPassword(!m_password.isEmpty()), m_home(home), m_ready(std::move(ready))
{}

RfbReader::~RfbReader() = default;

std::int64_t RfbReader::steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void RfbReader::feed(QByteArray bytes)
{
    const std::scoped_lock held(m_in);
    if (m_detached || bytes.isEmpty()) {
        return;
    }
    m_waitingBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
    m_waiting.push_back(Item{std::move(bytes)});
    wake();
}

void RfbReader::still(int asked, std::uint64_t number)
{
    const std::scoped_lock held(m_in);
    if (m_detached) {
        return;
    }
    Item item;
    item.still = true;
    item.asked = asked;
    item.number = number;
    m_waiting.push_back(std::move(item));
    wake();
}

void RfbReader::wake()
{
    if (m_running || m_detached || m_waiting.empty()) {
        return;
    }
    m_running = true;
    QThreadPool::globalInstance()->start([self = shared_from_this()] { self->run(); });
}

void RfbReader::run()
{
    for (;;) {
        Item item;
        {
            const std::scoped_lock held(m_in);
            if (m_detached || m_waiting.empty()) {
                m_running = false;
                return;
            }
            item = std::move(m_waiting.front());
            m_waiting.pop_front();
        }
        // Nothing leaves a thread flying (DESIGN.md section 7): what memory refuses ends the session.
        try {
            if (item.still) {
                makeStill(item);
            } else {
                readOne(item.bytes);
            }
        } catch (...) {
            m_over = true;
            // Under its lock, as every other reading and writing of what is told (round-5 review LOW 2).
            const std::scoped_lock held(m_out);
            try {
                m_taken.ended = base::Error{"no_memory", "There was no memory to read the server's screen into"};
                tell();
            } catch (...) {
                // (Nothing more is told: the screen ends a session whose reader tells nothing for long, the line held
                // back or not.)
                m_told = true;
            }
        }
        m_waitingBytes.fetch_sub(item.bytes.size(), std::memory_order_relaxed);
    }
}

void RfbReader::readOne(const QByteArray& bytes)
{
    if (m_over) {
        return; // (the connection is over: nothing more of it is read)
    }
    base::Result<void> fed = m_client.feed(
        std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), static_cast<std::size_t>(bytes.size())),
        steadyMs());
    // VNC authentication: the server waits for the answer to its challenge, which is made here, where the password
    // is (the web's handshake: the challenge read, then the answer, or no_password).
    // An answer that was made goes out, and is told as one that went, whatever the bytes after the challenge came to
    // (outside review 2.5: a server that sent its challenge and garbage in one breath had the answer sent uncounted).
    // As the web's: the challenge read, the answer written, and the result read after it.
    bool answered = false;
    if (fed.ok()) {
        if (const std::optional<std::array<std::uint8_t, 16>> asked = m_client.challenge()) {
            fed = answerTheChallenge(*asked, answered);
        }
    }
    // Kept no longer than it is needed: once it has answered, the handshake is over, or the session ended.
    if (!m_password.isEmpty() && (answered || !fed.ok() || m_client.ready())) {
        letGoOfThePassword();
    }
    const rfb::Bytes outgoing = m_client.takeOutgoing();
    const rfb::Client::Changed changed = m_client.take();
    const int width = m_client.width();
    const int height = m_client.height();
    m_over = !fed.ok();
    // The hand-over is brought up to the screen whole where its picture is not the screen's (another size, or the
    // screen has it), or the update changed the most of it: copied before the lock, with which the screen takes. And
    // all of it is told where the screen holds another size, or the update changed the size or the most of it.
    bool whole = false;
    bool all = false;
    {
        const std::scoped_lock held(m_out);
        if (m_home == nullptr) {
            return;
        }
        const bool most = changed.resized || mostOf(changed.rects, width, height);
        const bool any = width > 0 && (changed.resized || !changed.rects.empty());
        whole = any && (m_handedStale || m_handed.width != width || m_handed.height != height || most);
        all = any && (m_theirs != std::pair{width, height} || most);
    }
    std::vector<std::uint8_t> copied;
    if (whole) {
        copied = m_client.rgb();
    }
    const std::scoped_lock held(m_out);
    if (m_home == nullptr) {
        return;
    }
    m_taken.outgoing.append(reinterpret_cast<const char*>(outgoing.data()), static_cast<qsizetype>(outgoing.size()));
    m_taken.passwordAnswered = m_taken.passwordAnswered || answered;
    if (!fed) {
        m_taken.ended = fed.error();
    }
    if (m_client.ready() && !m_taken.ready) {
        m_taken.ready = true;
        m_taken.name = plainName(m_client.name());
        m_taken.minor = m_client.minor();
    }
    m_taken.width = width;
    m_taken.height = height;
    m_taken.updates = m_client.updates();
    m_taken.frames = m_client.frames();
    m_taken.messageSince = m_client.messageSince();
    m_taken.pausedForMs = m_client.pausedForMs(); // over its share: how long the screen should hold the line back
    if (whole) {
        m_handed = aspeed::Picture{width, height, std::move(copied)};
        m_handedStale = false;
    } else if (m_handedStale && width > 0 && (changed.resized || !changed.rects.empty())) {
        // The screen took the hand-over's picture meanwhile (between the two locks): what is left here is what it had,
        // brought up to the screen whole before what changed is told (rare: copied under the lock).
        m_handed = aspeed::Picture{width, height, m_client.rgb()};
        m_handedStale = false;
    } else if (width > 0) {
        // (Brought up to the screen by what changed, whatever is told of it: all of it may be told of a picture that
        // was brought up so.)
        for (const rfb::Rect& rect : changed.rects) {
            copy(m_client.rgb(), m_handed.rgb, width, aspeed::Rect{rect.x, rect.y, rect.width, rect.height});
        }
    }
    if (all) {
        m_all = true;
        m_taken.changed.clear();
    } else if (width > 0) {
        for (const rfb::Rect& rect : changed.rects) {
            if (!m_all) {
                m_taken.changed.push_back(aspeed::Rect{rect.x, rect.y, rect.width, rect.height});
            }
        }
        if (m_taken.changed.size() > rfb::mostChanged) {
            m_all = true;
            m_taken.changed.clear();
        }
    }
    tell();
}

void RfbReader::letGoOfThePassword()
{
    m_password = SecretText();
    m_holdsAPassword.store(false, std::memory_order_relaxed);
}

base::Result<void> RfbReader::answerTheChallenge(const std::array<std::uint8_t, 16>& challenge, bool& said)
{
    // Nothing goes where no password is held: the server is told nothing more, and the session ends.
    if (m_password.isEmpty()) {
        return base::Error{std::string(rfb::noPassword), "the server asks for a password, and none is saved for it"};
    }
    const base::Result<vnc::Response> response = vnc::answer(m_password.view(), challenge);
    letGoOfThePassword(); // (used: kept nowhere for longer than it was needed)
    if (!response) {
        return base::Error{"dropped",
                           "the answer to the server's password could not be made: " + response.error().message};
    }
    // (Said from here on, whatever reading on after it comes to: the client has it to go out before anything else.)
    said = true;
    return m_client.answer(response.value(), steadyMs());
}

void RfbReader::makeStill(const Item& item)
{
    QByteArray made;
    if (m_client.width() > 0) {
        made = base::screen::still<QByteArray>(m_client.width(), m_client.height(), m_client.rgb(), item.asked);
    }
    const std::scoped_lock held(m_out);
    if (m_home == nullptr) {
        return;
    }
    m_taken.stills.push_back({item.number, std::move(made)});
    tell();
}

void RfbReader::tell()
{
    if (!m_told && m_home != nullptr) {
        m_told = true;
        QMetaObject::invokeMethod(m_home, m_ready, Qt::QueuedConnection);
    }
}

RfbReader::Taken RfbReader::take(aspeed::Picture& into)
{
    const std::scoped_lock held(m_out);
    m_told = false;
    Taken taken;
    taken.outgoing = std::exchange(m_taken.outgoing, {});
    taken.ended = std::exchange(m_taken.ended, std::nullopt);
    taken.stills = std::exchange(m_taken.stills, {});
    taken.passwordAnswered = std::exchange(m_taken.passwordAnswered, false);
    taken.ready = m_taken.ready;
    taken.name = m_taken.name;
    taken.minor = m_taken.minor;
    taken.width = m_taken.width;
    taken.height = m_taken.height;
    taken.updates = m_taken.updates;
    taken.frames = m_taken.frames;
    taken.messageSince = m_taken.messageSince;
    taken.pausedForMs =
        std::exchange(m_taken.pausedForMs, std::int64_t{0}); // taken once: a still's take is no new pause
    // (What changed is told only of a hand-over brought up to the screen: readOne makes it so before it tells any.)
    if (m_handedStale || m_handed.rgb.empty() || (!m_all && m_taken.changed.empty())) {
        return taken; // (nothing of the screen changed since it last took)
    }
    std::vector<aspeed::Rect> changed = std::exchange(m_taken.changed, {});
    if (m_all || into.width != m_handed.width || into.height != m_handed.height) {
        // All of it: the screen is handed the picture itself, and what is left here is what it had (stale).
        taken.changed.push_back({0, 0, m_handed.width, m_handed.height});
        std::swap(into, m_handed);
        m_handedStale = true;
        m_all = false;
        m_theirs = {into.width, into.height};
        return taken;
    }
    for (const aspeed::Rect& rect : changed) {
        copy(m_handed.rgb, into.rgb, into.width, rect);
    }
    taken.changed = std::move(changed);
    return taken;
}

void RfbReader::detach()
{
    // First: a read under way stops at its next asking, without waiting for a lock (outside review 2.1).
    m_stopping.store(true, std::memory_order_relaxed);
    {
        const std::scoped_lock held(m_in);
        m_detached = true;
        m_waiting.clear();
    }
    const std::scoped_lock held(m_out);
    m_home = nullptr;
}

} // namespace qkvm::core
