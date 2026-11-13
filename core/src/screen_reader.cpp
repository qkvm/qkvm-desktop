#include "screen_reader.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
// Built with the standard library's own checks in every build, as server_screen.cpp is (core/CMakeLists.txt).
#if !_MSVC_STL_HARDENING_ARRAY || !_MSVC_STL_HARDENING_SPAN || !_MSVC_STL_HARDENING_VECTOR
#error "This holds what a management controller's screen came to, and is built with _MSVC_STL_HARDENING"
#endif

#include <QThreadPool>

#include <qkvm/base/screen_still.h>

namespace qkvm::core {

namespace aspeed = bmc::aspeed;

namespace {

std::int64_t steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::size_t rowsOf(int height)
{
    return static_cast<std::size_t>((std::max(height, 0) + 7) / 8);
}

// A rectangle that lies on a picture of that size (what a screen says it changed always does: looked at all the same,
// as everything is that copies by it).
bool on(const aspeed::Rect& rect, const aspeed::Picture& picture)
{
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 && rect.x <= picture.width - rect.width &&
           rect.y <= picture.height - rect.height &&
           picture.rgb.size() ==
               std::size_t{3} * static_cast<std::size_t>(picture.width) * static_cast<std::size_t>(picture.height);
}

// Whether what a frame changed is most of its picture: then it goes over whole, copied outside the lock.
bool mostOf(std::span<const aspeed::Rect> changed, const aspeed::Picture& picture)
{
    std::uint64_t area = 0;
    for (const aspeed::Rect& rect : changed) {
        area +=
            static_cast<std::uint64_t>(std::max(rect.width, 0)) * static_cast<std::uint64_t>(std::max(rect.height, 0));
    }
    const auto all = static_cast<std::uint64_t>(std::max(picture.width, 0)) *
                     static_cast<std::uint64_t>(std::max(picture.height, 0));
    return all > 0 && 2 * area >= all;
}

// Rows `top` to `top + high` of a picture, from `left` to `left + wide`, copied into another of the same size.
void copy(const aspeed::Picture& from, aspeed::Picture& into, int left, int wide, int top, int high)
{
    const auto across = std::size_t{3} * static_cast<std::size_t>(wide);
    const auto row = std::size_t{3} * static_cast<std::size_t>(from.width);
    const std::span<const std::uint8_t> source(from.rgb);
    const std::span<std::uint8_t> target(into.rgb);
    for (int y = top; y < top + high; ++y) {
        const std::size_t at = static_cast<std::size_t>(y) * row + std::size_t{3} * static_cast<std::size_t>(left);
        std::memcpy(target.subspan(at, across).data(), source.subspan(at, across).data(), across);
    }
}

} // namespace

ScreenReader::ScreenReader(QObject* home, std::function<void()> ready, int mostResizesASecond)
    : m_mostResizes(mostResizesASecond), m_home(home), m_ready(std::move(ready))
{}

ScreenReader::~ScreenReader() = default;

void ScreenReader::read(std::uint64_t session, bmc::ami::WholeFrame frame)
{
    const std::scoped_lock held(m_in);
    if (m_detached || session <= m_endedUpTo) {
        return;
    }
    Item item;
    item.session = session;
    item.frame = std::move(frame);
    m_waiting.push_back(std::move(item));
    wake();
}

void ScreenReader::still(std::uint64_t session, int asked, std::uint64_t number)
{
    const std::scoped_lock held(m_in);
    if (m_detached || session <= m_endedUpTo) {
        return;
    }
    Item item;
    item.session = session;
    item.still = true;
    item.asked = asked;
    item.number = number;
    m_waiting.push_back(std::move(item));
    wake();
}

void ScreenReader::wake()
{
    if (m_running || m_detached || m_waiting.empty()) {
        return;
    }
    m_running = true;
    QThreadPool::globalInstance()->start([self = shared_from_this()] { self->run(); });
}

void ScreenReader::run()
{
    for (;;) {
        Item item;
        std::unique_ptr<aspeed::Screen> old; // (let go of outside the lock: up to 24 MiB given back)
        bool over = false;
        {
            const std::scoped_lock held(m_in);
            if (m_detached || m_waiting.empty()) {
                // A screen of a session that is over is not kept until the next.
                if (m_detached || m_screenOf <= m_endedUpTo) {
                    old = std::move(m_screen);
                    m_screenOf = 0;
                }
                m_running = false;
                over = true;
            } else {
                item = std::move(m_waiting.front());
                m_waiting.pop_front();
                if (!item.still && item.session != m_screenOf) {
                    // A session's frames go into a picture of its own.
                    old = std::move(m_screen);
                    m_screen = std::make_unique<aspeed::Screen>();
                    m_screenOf = item.session;
                    m_resized.clear();
                    m_refused = false;
                }
            }
        }
        old.reset();
        if (over) {
            return;
        }
        // Nothing leaves a thread flying (DESIGN.md section 7): what memory refuses is a frame not read.
        try {
            if (item.still) {
                makeStill(item);
            } else {
                readOne(item);
            }
        } catch (...) {
            try {
                if (!item.still) {
                    handOver(item.session, Read{base::Error{"no_memory", "There was no memory to read it into"}},
                             nullptr, false);
                }
            } catch (...) {
                // Not even that: nothing more of this session is read, and its line falls quiet (the screen ends it
                // when nothing came for its quiet time).
                m_refused = true;
            }
        }
    }
}

void ScreenReader::readOne(Item& item)
{
    if (m_refused) {
        return; // (the session was left for its sizes: nothing more of it is read)
    }
    aspeed::Screen& screen = *m_screen;
    const bmc::aspeed::FrameHeader& header = item.frame.header;
    // What counts is a picture made anew (a header the screen refuses makes none, and changes nothing it holds).
    const bool resized = screen.resizedBy(header);
    if (resized) {
        const std::int64_t at = steadyMs();
        std::erase_if(m_resized, [at](std::int64_t when) { return at - when >= 1000; });
        m_resized.push_back(at);
        if (static_cast<int>(m_resized.size()) > m_mostResizes) {
            m_refused = true;
            Read refused{base::Error{"garbled", "The screen changes its size more often than a server's does"}, true};
            handOver(item.session, std::move(refused), nullptr, false);
            return;
        }
    }
    const bool first = screen.picture().rgb.empty();
    Read read{screen.read(header, item.frame.data)};
    item.frame.data = {}; // (megabytes, not kept beyond their reading)
    handOver(item.session, std::move(read), &screen, resized || first);
}

void ScreenReader::makeStill(const Item& item)
{
    QByteArray made;
    if (m_screen != nullptr && m_screenOf == item.session && !m_refused) {
        const aspeed::Picture& picture = m_screen->picture();
        made = base::screen::still<QByteArray>(picture.width, picture.height, picture.rgb, item.asked);
    }
    const std::scoped_lock held(m_out);
    if (m_home == nullptr || item.session <= m_endedOut) {
        return;
    }
    handOverOf(item.session);
    m_stills.push_back({item.number, std::move(made)});
    tell();
}

void ScreenReader::handOver(std::uint64_t session, Read read, const aspeed::Screen* screen, bool anew)
{
    // What the screen has to be told of: all of it where the picture was made anew, is of another size than the
    // screen's, or changed for the most part; else what this frame changed, beside what changed before since the
    // screen last took. The hand-over is brought up to the picture whole where that is all, or where what it held went
    // to the screen whole: copied before the lock is taken, with which the screen takes (a copy of 6 MB under it held
    // the screen up).
    bool all = false;
    bool whole = false;
    std::vector<std::uint8_t> copied;
    if (screen != nullptr) {
        const aspeed::Picture& picture = screen->picture();
        {
            const std::scoped_lock held(m_out);
            if (m_home == nullptr || session <= m_endedOut) {
                return;
            }
            const bool same = m_handedOf == session;
            all = !same || std::pair{picture.width, picture.height} != m_theirs;
            whole = !same || m_handedStale || picture.width != m_handed.width || picture.height != m_handed.height;
        }
        all = all || anew || mostOf(screen->changed(), picture);
        whole = whole || all;
        if (whole) {
            copied = picture.rgb;
        }
    }
    const std::scoped_lock held(m_out);
    if (m_home == nullptr || session <= m_endedOut) {
        return;
    }
    handOverOf(session);
    if (screen != nullptr) {
        const aspeed::Picture& picture = screen->picture();
        const bool sized = picture.width == m_handed.width && picture.height == m_handed.height;
        if (whole || m_handedStale || !sized) {
            if (!whole) {
                copied = picture.rgb; // (what was here went to the screen meanwhile: rare, and copied here after all)
            }
            m_handed.width = picture.width;
            m_handed.height = picture.height;
            m_handed.rgb = std::move(copied);
            m_handedStale = false;
        }
        if (all || std::pair{picture.width, picture.height} != m_theirs) {
            m_spans.assign(rowsOf(picture.height), {0, picture.width});
        } else {
            if (m_spans.size() != rowsOf(picture.height)) {
                m_spans.assign(rowsOf(picture.height), {picture.width, 0});
            }
            for (const aspeed::Rect& rect : screen->changed()) {
                if (!on(rect, picture) || !on(rect, m_handed)) {
                    continue;
                }
                if (!whole) {
                    copy(picture, m_handed, rect.x, rect.width, rect.y, rect.height);
                }
                for (int row = rect.y / 8; row <= (rect.y + rect.height - 1) / 8; ++row) {
                    std::pair<int, int>& span = m_spans.at(static_cast<std::size_t>(row));
                    span.first = std::min(span.first, rect.x);
                    span.second = std::max(span.second, rect.x + rect.width);
                }
            }
        }
    }
    m_reads.push_back(std::move(read));
    tell();
}

void ScreenReader::handOverOf(std::uint64_t session)
{
    if (m_handedOf == session) {
        return;
    }
    m_handedOf = session;
    m_handed = {};
    m_handedStale = false;
    m_theirs = {};
    m_spans.clear();
    m_reads.clear();
    m_stills.clear();
}

void ScreenReader::tell()
{
    if (!m_told && m_home != nullptr) {
        m_told = true;
        QMetaObject::invokeMethod(m_home, m_ready, Qt::QueuedConnection);
    }
}

ScreenReader::Taken ScreenReader::take(std::uint64_t session, aspeed::Picture& into)
{
    Taken taken;
    const std::scoped_lock held(m_out);
    m_told = false;
    if (m_handedOf != session) {
        return taken;
    }
    taken.reads = std::exchange(m_reads, {});
    taken.stills = std::exchange(m_stills, {});
    if (m_handedStale || m_handed.rgb.empty()) {
        return taken; // (the screen has the newest picture already, and nothing of it changed since)
    }
    const bool all = std::ranges::all_of(
        m_spans, [this](const std::pair<int, int>& span) { return span.first == 0 && span.second == m_handed.width; });
    if (all || into.width != m_handed.width || into.height != m_handed.height) {
        // All of it to be told: the screen is handed the picture itself, not a copy (6 MB at 1080p, not copied on its
        // thread). What is left here is what the screen had: brought up to the picture whole by the next frame.
        taken.changed.push_back({0, 0, m_handed.width, m_handed.height});
        std::swap(into, m_handed);
        m_handedStale = true;
        m_theirs = {into.width, into.height};
        m_spans.assign(rowsOf(into.height), {into.width, 0});
        return taken;
    }
    for (std::size_t row = 0; row < m_spans.size(); ++row) {
        const auto [from, to] = m_spans[row];
        if (to <= from) {
            continue;
        }
        const int top = static_cast<int>(row) * 8;
        const int high = std::min(8, m_handed.height - top);
        copy(m_handed, into, from, to - from, top, high);
        aspeed::Rect* const last = taken.changed.empty() ? nullptr : &taken.changed.back();
        if (last != nullptr && last->x == from && last->width == to - from && last->y + last->height == top) {
            last->height += high;
        } else {
            taken.changed.push_back({from, top, to - from, high});
        }
    }
    std::ranges::fill(m_spans, std::pair{m_handed.width, 0});
    return taken;
}

void ScreenReader::end(std::uint64_t session)
{
    std::unique_ptr<aspeed::Screen> old;
    {
        const std::scoped_lock held(m_out);
        m_endedOut = std::max(m_endedOut, session);
        if (m_handedOf <= m_endedOut) {
            handOverOf(0);
        }
    }
    {
        const std::scoped_lock held(m_in);
        m_endedUpTo = std::max(m_endedUpTo, session);
        std::erase_if(m_waiting, [this](const Item& one) { return one.session <= m_endedUpTo; });
        if (m_screen != nullptr && m_screenOf <= m_endedUpTo) {
            if (m_running) {
                m_screen->giveUp(); // (let go of by its thread, which sees that the session is over)
            } else {
                old = std::move(m_screen);
                m_screenOf = 0;
            }
        }
    }
}

void ScreenReader::detach()
{
    std::unique_ptr<aspeed::Screen> old;
    {
        const std::scoped_lock held(m_out);
        m_home = nullptr;
        m_ready = nullptr;
        handOverOf(0);
    }
    {
        const std::scoped_lock held(m_in);
        m_detached = true;
        m_waiting.clear();
        if (m_screen != nullptr) {
            if (m_running) {
                m_screen->giveUp();
            } else {
                old = std::move(m_screen);
                m_screenOf = 0;
            }
        }
    }
}

} // namespace qkvm::core
