#include <qkvm/core/jetkvm_live.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

#include <QTimer>

#include <qkvm/base/clock.h>
#include <qkvm/base/h264.h>
#include <qkvm/core/jetkvm_connection.h>
#include <qkvm/core/jetkvm_viewer.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {
constexpr int powerKey = 0x66;
constexpr int farthest = 32767;
constexpr int mostStep = 127;
const QString inUseCode = u"in_use"_s;
const QString anotherCode = u"another_machine"_s;
const QString inUseSentence =
    u"Someone is using this JetKVM's own console. It shows its picture to one viewer at a time."_s;
} // namespace

// One window's Live of a JetKVM: what it is asked goes to the one JetKvmLive all share, while that knows it.
class JetKvmLive::Handle : public Live {
public:
    explicit Handle(JetKvmLive* live) : m_live(live) {}
    ~Handle() override { leave(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) = delete;
    Handle& operator=(Handle&&) = delete;

    bool isOpen() const override { return m_live != nullptr; }
    QString transport() const override { return m_live != nullptr ? u"webrtc"_s : QString(); }
    // "in_use": another viewer has the JetKVM, and only "Show here" takes it back. "another_machine": another JetKVM
    // answers at its address, and only a change of the card takes it back (J5).
    bool endsForGood(const QString& code) const override { return code == inUseCode || code == anotherCode; }
    void setNeed(Need /*need*/) override {}
    void askForAKeyFrame() override
    {
        if (m_live != nullptr && m_live->m_viewer != nullptr) {
            m_live->m_viewer->askForAKeyFrame();
        }
    }
    void askControl(bool on) override
    {
        if (m_live != nullptr) {
            m_live->askControl(this, on);
        }
    }
    bool hasControl() const override { return m_live != nullptr && m_live->holds(this); }
    void key(int usage, bool down) override
    {
        if (m_live != nullptr && m_live->holds(this)) {
            m_live->key(usage, down);
        }
    }
    void pointerAt(int x, int y) override
    {
        if (m_live != nullptr && m_live->holds(this)) {
            m_live->pointerAt(x, y);
        }
    }
    void pointerBy(int dx, int dy) override
    {
        if (m_live != nullptr && m_live->holds(this)) {
            m_live->pointerBy(dx, dy);
        }
    }
    void button(int number, bool down) override
    {
        if (m_live != nullptr && m_live->holds(this)) {
            m_live->button(number, down);
        }
    }
    void wheel(int dx, int dy) override
    {
        if (m_live != nullptr && m_live->holds(this)) {
            m_live->wheel(dx, dy);
        }
    }
    void releaseAll() override
    {
        if (m_live != nullptr && m_live->holds(this)) {
            m_live->releaseAll();
        }
    }
    void close() override
    {
        m_closed = true;
        leave();
    }

    void over() { m_live.clear(); }
    bool wasClosed() const { return m_closed; }

private:
    void leave()
    {
        JetKvmLive* live = m_live.data();
        m_live.clear();
        if (live != nullptr) {
            live->leave(this);
        }
    }

    QPointer<JetKvmLive> m_live;
    bool m_closed = false;
};

JetKvmLive::JetKvmLive(JetKvmViewer& viewer, QObject* parent) : OwnLive(parent), m_viewer(&viewer)
{
    connect(&viewer, &JetKvmViewer::pictureCame, this, &JetKvmLive::handOn);
    connect(&viewer, &JetKvmViewer::steppedAside, this, [this] { closeAll(inUseCode, inUseSentence); });
    // Another JetKVM at its address: there is nothing of this one's to show, until the card is changed (J5).
    connect(&viewer, &JetKvmViewer::anotherAnswers, this,
            [this](const QString& message) { closeAll(anotherCode, message); });
    // Keyboard and mouse are held on one connection, never carried over to the next (rule 24): where it is over,
    // whoever held them holds nothing, and is told so.
    connect(&viewer, &JetKvmViewer::connectionOver, this, [this](quint64 serial) {
        if (m_holder != nullptr && m_given && serial == m_onSerial) {
            loseControl(base::Error{"dropped", "The connection to the JetKVM ended. Take keyboard and mouse again."});
        }
    });
}

JetKvmLive::~JetKvmLive()
{
    // Gone under whoever has it live (the program ending): what is down is let go of as far as that goes, and nobody
    // is told from in here.
    if (m_holder != nullptr && m_given) {
        releaseAll();
    }
    for (const Watcher& watcher : std::exchange(m_watchers, {})) {
        watcher.handle->over();
        if (m_viewer != nullptr) {
            m_viewer->windowLeft();
        }
    }
}

std::unique_ptr<Live> JetKvmLive::open(Live::Need /*need*/)
{
    auto handle = std::make_unique<Handle>(this);
    Handle* made = handle.get();
    const bool aside = m_viewer != nullptr && m_viewer->aside();
    if (m_over || m_viewer == nullptr || aside) {
        // Ended already, or another viewer has it: told so a turn after whoever asked holds it.
        made->over();
        const QString code = aside && !m_over ? inUseCode : m_overCode;
        const QString message = aside && !m_over ? inUseSentence : m_overMessage;
        QTimer::singleShot(0, made, [made, code, message] {
            if (!made->wasClosed()) {
                emit made->closed(code, message);
            }
        });
        return handle;
    }
    m_watchers.push_back(Watcher{made});
    m_viewer->windowCame();
    QTimer::singleShot(0, made, [made] {
        if (made->isOpen()) {
            emit made->opened();
        }
    });
    return handle;
}

void JetKvmLive::end(const base::Error& why)
{
    if (std::exchange(m_over, true)) {
        return;
    }
    m_overCode = QString::fromStdString(why.code);
    m_overMessage = QString::fromStdString(why.message);
    closeAll(m_overCode, m_overMessage);
    m_viewer.clear();
}

void JetKvmLive::leave(Handle* handle)
{
    const auto gone = std::ranges::find(m_watchers, handle, &Watcher::handle);
    if (gone == m_watchers.end()) {
        return;
    }
    // Whoever held keyboard and mouse there: everything down is let go of first, behind all it said.
    if (m_holder == handle) {
        if (m_given) {
            releaseAll();
        }
        dropHolder();
    }
    m_watchers.erase(gone);
    if (m_viewer != nullptr) {
        m_viewer->windowLeft();
    }
}

void JetKvmLive::askControl(Handle* handle, bool on)
{
    // One word for each asking and each giving back, in their order (live.h), as a kvmd's KvmdLive says them.
    const bool watches = std::ranges::find(m_watchers, handle, &Watcher::handle) != m_watchers.end();
    if (!on) {
        if (m_holder == handle) {
            const bool given = m_given;
            // Everything that is down is let go of first, behind everything said before: one connection takes them in
            // order, so whatever the next holder or an action sends lands after it (rule 27).
            if (given) {
                releaseAll();
            }
            dropHolder();
            if (!given) {
                tellLater(handle, u"given_up"_s,
                          u"Keyboard and mouse were given back before the JetKVM had given them."_s);
            }
            tellLater(handle, {}, {});
        } else if (watches) {
            tellLater(handle, {}, {});
        }
        return;
    }
    if (m_holder == handle || !watches || m_viewer == nullptr) {
        return;
    }
    if (m_holder != nullptr) {
        tellLater(handle, inUseCode,
                  u"Keyboard and mouse of this machine are in use in another window. Release them there first."_s);
        return;
    }
    m_holder = handle;
    const quint64 turn = ++m_controlTurn;
    m_viewer->inputReady([self = QPointer<JetKvmLive>(this), turn](const base::Result<QPointer<JetKvmConnection>>& on) {
        if (self == nullptr || turn != self->m_controlTurn || self->m_holder == nullptr) {
            return;
        }
        Handle* holder = self->m_holder;
        if (!on || on.value() == nullptr) {
            self->dropHolder();
            const base::Error why = on ? base::Error{"dropped", "The connection to the JetKVM ended"} : on.error();
            emit holder->controlChanged(false, QString::fromStdString(why.code), QString::fromStdString(why.message));
            return;
        }
        self->m_on = on.value();
        self->m_onSerial = on.value()->serial();
        self->m_given = true;
        self->m_holderNumber = ++self->m_holders;
        emit holder->controlChanged(true, {}, {});
    });
}

bool JetKvmLive::holds(const Handle* handle) const
{
    return m_holder == handle && m_given && m_on != nullptr;
}

void JetKvmLive::key(int usage, bool down)
{
    // (The keyboard's Power key is never sent to a machine: it shuts it down without the question a power action is
    // asked first, rule 27.)
    if (usage == powerKey) {
        return;
    }
    if (m_keyboard.take(usage, down)) {
        JetKvmConnection::Report report;
        report.keyboard = keyboardNow();
        say(report);
    }
}

void JetKvmLive::pointerAt(int x, int y)
{
    m_placed = true;
    m_x = std::clamp(x, 0, farthest);
    m_y = std::clamp(y, 0, farthest);
    say(JetKvmConnection::Report{JetKvmConnection::Report::Kind::Place, {}, m_x, m_y, m_buttons}, true);
}

void JetKvmLive::pointerBy(int dx, int dy)
{
    // (Moved by steps from here on: a button goes with a step, until it is put at a place again.)
    m_placed = false;
    while ((dx != 0 || dy != 0) && m_holder != nullptr) {
        const int stepX = std::clamp(dx, -mostStep, mostStep);
        const int stepY = std::clamp(dy, -mostStep, mostStep);
        say(JetKvmConnection::Report{JetKvmConnection::Report::Kind::Step, {}, stepX, stepY, m_buttons}, true);
        dx -= stepX;
        dy -= stepY;
    }
}

void JetKvmLive::button(int number, bool down)
{
    if (number < 1 || number > 5) {
        return;
    }
    const int bit = 1 << (number - 1);
    const int buttons = down ? m_buttons | bit : m_buttons & ~bit;
    if (buttons == m_buttons) {
        return;
    }
    m_buttons = buttons;
    sayMouse(false);
}

void JetKvmLive::wheel(int dx, int dy)
{
    while ((dx != 0 || dy != 0) && m_holder != nullptr) {
        const int turnX = std::clamp(dx, -mostStep, mostStep);
        const int turnY = std::clamp(dy, -mostStep, mostStep);
        say(JetKvmConnection::Report{JetKvmConnection::Report::Kind::Wheel, {}, turnX, turnY, 0}, true);
        dx -= turnX;
        dy -= turnY;
    }
}

void JetKvmLive::releaseAll()
{
    // Nothing down, as lettings go: never refused for this holder. (A key-up alone is no letting go here: a report is
    // the whole keyboard.)
    m_keyboard.clear();
    m_buttons = 0;
    JetKvmConnection::Report nothing;
    say(nothing, false, true);
    sayMouse(true);
}

void JetKvmLive::sayMouse(bool lettingGo)
{
    if (m_placed) {
        say(JetKvmConnection::Report{JetKvmConnection::Report::Kind::Place, {}, m_x, m_y, m_buttons}, false, lettingGo);
    } else {
        say(JetKvmConnection::Report{JetKvmConnection::Report::Kind::Step, {}, 0, 0, m_buttons}, false, lettingGo);
    }
}

JetKvmConnection::Keyboard JetKvmLive::keyboardNow() const
{
    JetKvmConnection::Keyboard keyboard;
    keyboard.modifiers = m_keyboard.now().modifiers;
    for (const std::uint8_t key : m_keyboard.now().keys) {
        keyboard.keys.append(key);
    }
    return keyboard;
}

void JetKvmLive::say(const JetKvmConnection::Report& report, bool fold, bool lettingGo)
{
    if (m_holder == nullptr || !m_given) {
        return;
    }
    if (m_on == nullptr) {
        loseControl(base::Error{"dropped", "The connection to the JetKVM ended. Take keyboard and mouse again."});
        return;
    }
    const std::optional<base::Error> failed = m_on->say(report, m_holderNumber, fold, lettingGo);
    if (failed && !lettingGo) {
        loseControl(*failed);
    }
}

void JetKvmLive::loseControl(const base::Error& why)
{
    Handle* held = m_holder;
    const bool down = !m_keyboard.now().keys.empty() || m_keyboard.now().modifiers != 0 || m_buttons != 0;
    dropHolder();
    if (held != nullptr) {
        tellLater(held, QString::fromStdString(why.code), QString::fromStdString(why.message), down);
    }
}

void JetKvmLive::dropHolder()
{
    m_holder = nullptr;
    m_given = false;
    m_on = nullptr;
    m_onSerial = 0;
    ++m_controlTurn;
    m_keyboard.clear();
    m_buttons = 0;
    m_placed = false;
}

void JetKvmLive::tellLater(Handle* handle, const QString& code, const QString& message, bool leftDown)
{
    // Said a turn later, never from inside the call (a refusal, a giving back, a loss): whoever holds the handle may be
    // in the middle of its own work.
    QTimer::singleShot(0, handle, [handle, code, message, leftDown] {
        if (handle->wasClosed() || !handle->isOpen()) {
            return;
        }
        if (leftDown) {
            emit handle->leftDown();
        }
        emit handle->controlChanged(false, code, message);
    });
}

void JetKvmLive::closeAll(const QString& code, const QString& message)
{
    // (Whoever held keyboard and mouse: let go of first, as far as that still goes.)
    if (m_holder != nullptr) {
        if (m_given) {
            releaseAll();
        }
        dropHolder();
    }
    for (const Watcher& watcher : std::exchange(m_watchers, {})) {
        Handle* handle = watcher.handle;
        handle->over();
        if (m_viewer != nullptr) {
            m_viewer->windowLeft();
        }
        // Said a turn later: whoever ends a live may be in the middle of its own work.
        QTimer::singleShot(0, handle, [handle, code, message] {
            if (!handle->wasClosed()) {
                emit handle->closed(code, message);
            }
        });
    }
}

namespace {

// No picture larger than the engine shows (its maxPictureWidth and maxPictureHeight, engine/types.h) says a size.
constexpr std::uint64_t widest = 4096;
constexpr std::uint64_t tallest = 2304;

} // namespace

void JetKvmLive::handOn(const QByteArray& annexB, bool key, qint64 arrivedUs)
{
    Live::Picture picture;
    picture.h264 = true;
    picture.keyFrame = key;
    picture.takenUs = base::steadyNowUs();
    picture.data = annexB;
    picture.arrivedUs = arrivedUs;
    // Its size, as a key frame's SPS says what of it is shown: said once it is known, and again where it changes.
    bool resized = false;
    if (key) {
        const std::optional<std::pair<int, int>> said = base::h264::shownSize(
            std::as_bytes(std::span(annexB.constData(), static_cast<std::size_t>(annexB.size()))), widest, tallest);
        if (said && *said != m_size) {
            m_size = *said;
            resized = true;
        }
    }
    const Live::Video video{true, m_size.first, m_size.second};
    const QPointer<JetKvmLive> self(this);
    std::vector<QPointer<Handle>> all;
    all.reserve(m_watchers.size());
    for (const Watcher& watcher : m_watchers) {
        all.emplace_back(watcher.handle);
    }
    for (const QPointer<Handle>& handle : all) {
        if (self == nullptr) {
            return;
        }
        const auto watcher = std::ranges::find(m_watchers, handle.data(), &Watcher::handle);
        if (handle == nullptr || watcher == m_watchers.end()) {
            continue;
        }
        if (!watcher->told || resized) {
            if (!key) {
                continue; // from its first key frame
            }
            watcher->told = true;
            emit handle->videoChanged(video);
            if (self == nullptr) {
                return;
            }
            if (handle == nullptr) {
                continue;
            }
        }
        const auto still = std::ranges::find(m_watchers, handle.data(), &Watcher::handle);
        if (still == m_watchers.end()) {
            continue;
        }
        picture.counter = ++still->counter;
        emit handle->pictureCame(picture);
    }
}

} // namespace qkvm::core
