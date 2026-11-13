#include <qkvm/core/server_live.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <qkvm/base/clock.h>
#include <qkvm/base/screen_changes.h>

using namespace Qt::StringLiterals;

namespace qkvm::core {

namespace {

// A turn of the wheel goes as one report a notch, so many at the most (the web's MOST_NOTCHES).
constexpr int mostNotches = 16;
// The far edge of the screen, as a live line counts the pointer.
constexpr int farthest = 32767;

} // namespace

ServerLive::ServerLive(ServerScreen* screen, Limits limits, QObject* parent)
    : Live(parent), m_screen(screen), m_limits(limits)
{
    if (m_screen != nullptr) {
        // A session that ends: the next begins with the whole screen, and keyboard and mouse went with it.
        connect(m_screen, &ServerScreen::ended, this, [this](const QString& code, const QString& message) {
            m_wholeNext = true;
            lose(code.isEmpty() ? u"dropped"_s : code, message);
            m_lettingGo = false; // (a controller lets go of what a session had down as it ends)
        });
        // Something this one let go of may have stayed down: told while it holds keyboard and mouse, and after they
        // went (given back, taken away, closed from here) where it had something down then.
        connect(m_screen, &ServerScreen::leftDown, this, [this] {
            if (m_turn || m_lettingGo) {
                emit leftDown();
            }
        });
        // The screen thrown away: there is no more of the server live here.
        connect(m_screen, &QObject::destroyed, this, [this] {
            if (!m_closed) {
                m_closed = true;
                m_looks.stop();
                m_turn.reset();
                emit closed(u"no_picture"_s, u"This server's screen is not looked at any more"_s);
            }
        });
    }
    connect(&m_looks, &QTimer::timeout, this, [this] { look(); });
    m_looks.start(m_limits.lookEveryMs);
    // The screen as it stands, a turn of the loop later (whoever made this has heard what it says by then).
    QTimer::singleShot(0, this, [this] {
        look();
        if (m_screen != nullptr && !m_closed) {
            sayWhole(m_screen->picture());
        }
    });
}

ServerLive::~ServerLive()
{
    if (!m_closed) {
        releaseAll();
    }
}

void ServerLive::shown(const bmc::aspeed::Picture& picture, std::span<const bmc::aspeed::Rect> changed)
{
    if (m_closed) {
        return;
    }
    if (m_wholeNext || std::pair{picture.width, picture.height} != m_size) {
        sayWhole(picture);
        return;
    }
    if (changed.empty() || changed.size() > static_cast<std::size_t>(base::screen::mostRects)) {
        if (!changed.empty()) {
            sayWhole(picture);
        }
        return;
    }
    std::vector<base::screen::Rect> rects;
    rects.reserve(changed.size());
    for (const bmc::aspeed::Rect& rect : changed) {
        rects.push_back({rect.x, rect.y, rect.width, rect.height});
    }
    QByteArray packet = base::screen::write<QByteArray>(picture.width, picture.height, picture.rgb, rects);
    if (packet.isEmpty()) {
        // What the frame changed cannot be said so (a rectangle outside the screen, say): the whole screen is, rather
        // than nothing (what it changed would be lost until the next whole).
        sayWhole(picture);
        return;
    }
    say(std::move(packet),
        rects.size() == 1 && rects.front() == base::screen::Rect{0, 0, picture.width, picture.height});
}

void ServerLive::sayWhole(const bmc::aspeed::Picture& picture)
{
    QByteArray packet = base::screen::whole<QByteArray>(picture.width, picture.height, picture.rgb);
    if (packet.isEmpty()) {
        return; // no picture yet (or none of a size there is room for): the next frame goes whole
    }
    if (std::pair{picture.width, picture.height} != m_size) {
        m_size = {picture.width, picture.height};
        Video video;
        video.changes = true;
        video.width = picture.width;
        video.height = picture.height;
        emit videoChanged(video);
    }
    m_wholeNext = false;
    say(std::move(packet), true);
}

void ServerLive::say(QByteArray packet, bool whole)
{
    Picture picture;
    picture.changes = true;
    picture.keyFrame = whole;
    picture.counter = ++m_counter;
    picture.takenUs = base::steadyNowUs();
    picture.arrivedUs = picture.takenUs;
    picture.data = std::move(packet);
    emit pictureCame(picture);
}

bool ServerLive::isOpen() const
{
    return !m_closed && m_screen != nullptr && m_size.first > 0;
}

void ServerLive::setNeed(Need /*need*/)
{
    // The console sends the screen as it is, at its own pace: there is nothing to ask of it.
}

void ServerLive::askForAKeyFrame()
{
    if (m_screen != nullptr && !m_closed) {
        sayWhole(m_screen->picture());
    }
}

void ServerLive::look()
{
    if (m_closed || m_screen == nullptr) {
        return;
    }
    static_cast<void>(m_screen->look());
    // The turn is over where the console went to somebody else or ended: said as soon as it is seen.
    if (m_turn) {
        if (const base::Result<void> still = m_screen->inTurn(*m_turn); !still) {
            lose(QString::fromStdString(still.error().code), QString::fromStdString(still.error().message));
        }
    }
}

void ServerLive::tell(bool on, const QString& code, const QString& message)
{
    QTimer::singleShot(0, this, [this, on, code, message] {
        if (!m_closed) {
            emit controlChanged(on, code, message);
        }
    });
}

void ServerLive::lose(const QString& code, const QString& message)
{
    if (!m_turn) {
        return;
    }
    // (What was down then was let go of by the screen, as the console went: watched there, as every letting go.)
    m_lettingGo = m_lettingGo || m_keyboard.now() != typed::Report{} || m_buttons != 0;
    m_turn.reset();
    m_keyboard.clear();
    m_buttons = 0;
    tell(false, code, message);
}

void ServerLive::askControl(bool on)
{
    if (m_closed) {
        return;
    }
    if (!on) {
        if (m_asking) {
            m_givenBackWhileAsking = true; // (answered when the asking is: a no for it, and then this)
            return;
        }
        if (m_turn) {
            releaseAll();
            m_turn.reset();
        }
        tell(false, {}, {});
        return;
    }
    if (m_turn || m_asking) {
        return; // (one asking at a time: no second word)
    }
    if (m_screen == nullptr) {
        tell(false, u"no_picture"_s, u"This server's screen is not looked at any more"_s);
        return;
    }
    m_asking = true;
    m_givenBackWhileAsking = false;
    m_screen->askForInput([self = QPointer<ServerLive>(this)](base::Result<int> turn) {
        // Decided a turn of the loop later: the screen may answer from inside the asking (its session holds the
        // console already), before a giving back said right after the asking.
        if (self == nullptr) {
            return;
        }
        QTimer::singleShot(0, self.data(), [self, turn = std::move(turn)] {
            if (self != nullptr) {
                self->answered(turn);
            }
        });
    });
}

void ServerLive::answered(const base::Result<int>& turn)
{
    if (m_closed) {
        return;
    }
    m_asking = false;
    const bool givenBack = std::exchange(m_givenBackWhileAsking, false);
    // (The turn may have ended in the turn of the loop it waited: the console went meanwhile.)
    base::Result<void> still;
    if (turn && m_screen == nullptr) {
        still = base::Error{"dropped", "The server's console was closed"};
    } else if (turn) {
        still = m_screen->inTurn(turn.value());
    }
    if (!turn) {
        tell(false, QString::fromStdString(turn.error().code), QString::fromStdString(turn.error().message));
    } else if (givenBack) {
        tell(false, u"given_up"_s, u"Keyboard and mouse were given back before they were given"_s);
    } else if (!still) {
        tell(false, QString::fromStdString(still.error().code), QString::fromStdString(still.error().message));
    } else {
        m_turn = turn.value();
        m_keyboard.clear();
        m_buttons = 0;
        tell(true, {}, {});
        return;
    }
    if (givenBack) {
        tell(false, {}, {});
    }
}

void ServerLive::keep(const base::Result<void>& said)
{
    if (said || !m_turn || m_screen == nullptr) {
        return;
    }
    // Refused because the turn is over (the console went, or ended): keyboard and mouse went with it. Refused for what
    // waits to leave (the console took nothing for a while): the turn stands.
    if (const base::Result<void> still = m_screen->inTurn(*m_turn); !still) {
        lose(QString::fromStdString(still.error().code), QString::fromStdString(still.error().message));
    }
}

void ServerLive::key(int usage, bool down)
{
    if (!m_turn || m_screen == nullptr || !m_keyboard.take(usage, down)) {
        return;
    }
    keep(m_screen->keyboard(m_keyboard.now(), *m_turn));
}

void ServerLive::sayMouse(int wheel)
{
    if (!m_turn || m_screen == nullptr) {
        return;
    }
    keep(m_screen->mouse(m_buttons, m_place.first, m_place.second, wheel, *m_turn));
}

void ServerLive::pointerAt(int x, int y)
{
    m_place = {std::clamp(x, 0, farthest), std::clamp(y, 0, farthest)};
    sayMouse(0);
}

void ServerLive::pointerBy(int dx, int dy)
{
    // Stepped over the screen by as much of it as the move is in pixels (the web's); before a picture, no move.
    if (m_size.first < 2 || m_size.second < 2) {
        return;
    }
    const auto step = [](int at, int by, int side) {
        return std::clamp(static_cast<int>(at + static_cast<long long>(by) * farthest / (side - 1)), 0, farthest);
    };
    m_place = {step(m_place.first, dx, m_size.first), step(m_place.second, dy, m_size.second)};
    sayMouse(0);
}

void ServerLive::button(int number, bool down)
{
    const std::uint8_t bit = typed::buttonBit(number);
    if (bit == 0) {
        return; // (back and forward have no bit in the server's mouse)
    }
    m_buttons = down ? static_cast<std::uint8_t>(m_buttons | bit) : static_cast<std::uint8_t>(m_buttons & ~bit);
    sayMouse(0);
}

void ServerLive::wheel(int /*dx*/, int dy)
{
    // One report a notch, up positive; the report has one wheel, so a sideways turn is not sent.
    for (int notch = 0; notch < std::min(std::abs(dy), mostNotches) && m_turn; ++notch) {
        sayMouse(dy > 0 ? 1 : -1);
    }
}

void ServerLive::releaseAll()
{
    const bool keys = m_keyboard.now() != typed::Report{};
    const bool buttons = m_buttons != 0;
    m_keyboard.clear();
    m_buttons = 0;
    if (!m_turn || m_screen == nullptr) {
        return;
    }
    m_lettingGo = m_lettingGo || keys || buttons;
    if (keys) {
        keep(m_screen->keyboard({}, *m_turn));
    }
    if (buttons) {
        sayMouse(0);
    }
}

void ServerLive::close()
{
    if (m_closed) {
        return;
    }
    releaseAll();
    m_turn.reset();
    m_closed = true;
    m_looks.stop();
}

} // namespace qkvm::core
