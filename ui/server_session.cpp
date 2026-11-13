#include "server_session.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QPointer>

#include <qkvm/core/server_client.h>

#include "alerts_view.h"
#include "app_lock.h"
#include "quick_access.h"
#include "video_host.h"
#include "wall_key.h"

using namespace Qt::StringLiterals;

namespace qkvm::ui {

namespace {

QString words(const char* text)
{
    return QCoreApplication::translate("Session", text);
}

QString incidentScope(const QUrl& address, const QString& account)
{
    return u"server-"_s + QString::fromLatin1(QCryptographicHash::hash(address.toEncoded() + '\n' + account.toUtf8(),
                                                                       QCryptographicHash::Sha256)
                                                  .toHex());
}

} // namespace

ServerSession::ServerSession(SessionView& view, WallView& wall, core::Settings& settings, core::SecretStore& secrets,
                             int statusEveryMs, QObject* parent)
    : QObject(parent), m_view(view), m_settings(settings), m_secrets(secrets), m_statusEveryMs(statusEveryMs),
      m_screen(secrets), m_server(serverId(), settings, secrets, statusEveryMs), m_merged(wall)
{
    // Whether this PC is a wall screen: the server says when it becomes one and when a person
    // signs it out, and the windows' wall only shows meanwhile. (The server is put on that wall
    // when this PC's own is, after it, or when it is first asked anything: takeTheServer.)
    m_server.markScreensIn(m_screen);
    m_merged.onlyShowWhile(m_screen);
    // Whose the sizes of the server's machines shown large are, on this PC (b10): the server's and the
    // account's at its wall, by the same reference as a favorite's.
    connect(&m_server, &Source::entered, this,
            [this](const core::Account& account) { m_scaleScope = incidentScope(m_server.address(), account.id); });
    connect(&m_server, &Source::left, this, [this] { m_scaleScope.clear(); });

    // What the screens ask: the server's conversation answers it, or the way between the walls.
    connect(&m_view, &SessionView::connectAsked, this, [this](const QString& address) {
        m_openWall = nullptr;
        connectTo(address);
    });
    connect(&m_view, &SessionView::signInAsked, this,
            [this](const QString& name, const QString& password, const QString& code, bool keep) {
                if (m_appLock == nullptr || !m_appLock->restricted()) {
                    m_server.signIn(name, password, code, keep);
                }
            });
    connect(&m_view, &SessionView::trustAsked, this, [this] {
        if (m_appLock == nullptr || !m_appLock->restricted()) {
            m_server.trust();
        }
    });
    connect(&m_view, &SessionView::pageAsked, this, [this] {
        // Only from the wall: before that there is nobody whose browser it would be for. Where no
        // browser opens, that is said in the first window: the program's icon asks for the page
        // too, and has no window of its own to say it in.
        if (isAtAServersWall()) {
            m_merged.openThePage(m_server.address());
        }
    });
    connect(&m_view, &SessionView::signOutAsked, this, [this] {
        m_openWall = nullptr;
        if (m_appLock != nullptr && m_appLock->wallScreen()) {
            m_appLock->ask(u"endScreen"_s);
        } else if (m_appLock == nullptr || !m_appLock->restricted()) {
            auto* coordinator = m_coordinator != nullptr ? m_coordinator : this;
            const auto address = m_server.address().toString();
            m_server.signOut();
            for (const auto& other : coordinator->m_windowSessions) {
                if (other != nullptr && other != this && other->m_foregroundAddress == address) {
                    other->m_openWall = nullptr;
                    other->m_server.leave();
                }
            }
            coordinator->stopBackgroundAt(address);
        }
    });
    connect(&m_view, &SessionView::anotherServerAsked, this, [this] {
        if (m_appLock != nullptr && m_appLock->restricted())
            return;
        rememberSource(u"connect"_s);
        m_openWall = nullptr;
        m_foregroundAddress.clear();
        if (onOwnWall()) {
            // From this PC's own wall to saying which server.
            leaveOwnWall();
            return;
        }
        m_server.leave();
    });
    connect(&m_view, &SessionView::ownWallAsked, this, [this] {
        if (m_appLock != nullptr && m_appLock->restricted())
            return;
        m_openWall = nullptr;
        enterOwnWall();
    });
    connect(&m_view, &SessionView::stopTryingAsked, &m_server, &ServerSource::stopTryingAgain);
    connect(&m_view, &SessionView::sourceAsked, this, &ServerSession::selectSource);
    connect(&m_view, &SessionView::machineAsked, this,
            [this, target = QPointer<WallView>(&wall)](const QString& scope, const QString& machine) {
                if (mayChooseSource() && target != nullptr && !machine.isEmpty())
                    openIncident(scope, machine, *target);
            });

    // What the server's conversation says happened, beside what the windows' wall does with it:
    // what the screens are shown (and which wall is remembered as the one shown: takeTheServer).
    connect(&m_server, &ServerSource::changed, this, &ServerSession::show, Qt::DirectConnection);
    show();
}

bool ServerSession::mayChooseSource() const
{
    return !m_monitoringStopped && !m_screen.isOne() && (m_appLock == nullptr || !m_appLock->restricted());
}

QString ServerSession::selectedSource() const
{
    if (!m_selectedSource.isEmpty())
        return m_selectedSource;
    return onOwnWall() ? u"pc"_s : m_foregroundAddress.isEmpty() ? u"connect"_s : m_foregroundAddress;
}

void ServerSession::rememberSource(const QString& source)
{
    m_selectedSource = source;
    if (m_slot != nullptr) {
        auto kept = m_slot->remembered();
        kept.source = source;
        static_cast<void>(m_slot->remember(kept));
    }
    showSources();
}

void ServerSession::showSources()
{
    QVariantList choices;
    const bool allowed = mayChooseSource();
    if (allowed) {
        if (m_pc != nullptr)
            choices.append(QVariantMap{{u"value"_s, u"pc"_s}, {u"label"_s, words("This PC")}});
        auto addresses = m_settings.remembered().servers;
        const auto selected = selectedSource();
        if (selected != u"pc"_s && selected != u"connect"_s && !addresses.contains(selected))
            addresses.prepend(selected);
        for (const auto& address : addresses)
            choices.append(QVariantMap{{u"value"_s, address}, {u"label"_s, QUrl(address).authority()}});
        choices.append(QVariantMap{{u"value"_s, u"connect"_s}, {u"label"_s, words("Connect to another server…")}});
    }
    m_view.showSources(choices, allowed ? selectedSource() : QString(), allowed);
    QVariantList tree;
    auto* coordinator = m_coordinator != nullptr ? m_coordinator : this;
    for (const auto& choice : choices) {
        const auto entry = choice.toMap();
        const auto source = entry.value(u"value"_s).toString();
        if (source == u"connect"_s)
            continue;
        const auto scope = source == u"pc"_s ? source : coordinator->m_alertAddresses.key(source);
        const auto branch = m_alerts != nullptr ? m_alerts->treeSource(scope) : QVariantMap();
        tree.append(QVariantMap{{u"key"_s, scope.isEmpty() ? source : scope},
                                {u"depth"_s, 0},
                                {u"source"_s, source},
                                {u"label"_s, entry.value(u"label"_s)},
                                {u"status"_s, branch.value(u"status"_s, words("Open to connect"))}});
        tree.append(branch.value(u"rows"_s).toList());
    }
    m_view.showSourceTree(tree);
}

void ServerSession::selectSource(const QString& source)
{
    if (!mayChooseSource())
        return;
    if (source == u"pc"_s) {
        m_openWall = nullptr;
        enterOwnWall();
    } else if (source == u"connect"_s) {
        m_view.anotherServer();
    } else if (const auto address = core::ServerClient::addressFrom(source);
               address && address.value().toString() == source) {
        m_openWall = nullptr;
        leaveOwnWall();
        // End the previous conversation before naming another server, including at sign-in.
        m_server.standAside();
        connectTo(source);
    }
}

std::unique_ptr<ServerSession> ServerSession::makeWindow(SessionView& view, WallView& wall, core::WindowSlot& kept)
{
    m_windowMode = true;
    auto child = std::make_unique<ServerSession>(view, wall, m_settings, m_secrets, m_statusEveryMs);
    child->m_coordinator = this;
    child->m_slot = &kept;
    child->m_merged.shareLive([this, session = child.get()](const Reached& at, core::Live::Need need, auto done) {
        if (at.source == PcSource::idOfThisPc()) {
            at.place->openLive(at.machine, need, std::move(done));
        } else {
            m_sharedLive.open(session->m_server.liveIdentity(), *at.place, session->m_server, at.machine, need,
                              std::move(done));
        }
    });
    child->m_merged.shareActions(m_sharedActions, [session = child.get()](const QString& key) {
        const auto reached = session->m_merged.reach(key);
        if (!reached)
            return key;
        const auto source = reached.source == PcSource::idOfThisPc() ? u"pc"_s : session->m_server.address().toString();
        return incidentScope(QUrl(source), {}) + u':' + reached.machine;
    });
    child->m_selectedSource = kept.remembered().source;
    if (child->m_selectedSource.isEmpty())
        child->m_selectedSource = m_settings.remembered().showsOwnWall || m_settings.remembered().server.isEmpty()
                                      ? u"pc"_s
                                      : m_settings.remembered().server;
    m_windowSessions.append(child.get());
    if (m_localWall != nullptr)
        child->useOwnWall(*m_localWall);
    if (m_appLock != nullptr)
        child->useAppLock(*m_appLock);
    if (m_alerts != nullptr)
        child->useAlerts(*m_alerts);
    if (m_shortcuts != nullptr)
        child->useQuickAccess(*m_shortcuts);
    child->openPagesWith(m_opener);
    connect(child.get(), &ServerSession::incidentDetailsAsked, this, &ServerSession::incidentDetailsAsked);
    connect(child.get(), &ServerSession::incidentUnavailable, this, &ServerSession::incidentUnavailable);
    connect(child.get(), &QObject::destroyed, this, [this] {
        m_windowSessions.removeIf([](const auto& child) { return child == nullptr; });
        QMetaObject::invokeMethod(this, &ServerSession::refreshMonitoring, Qt::QueuedConnection);
    });
    return child;
}

void ServerSession::beginWindow()
{
    if (m_coordinator == nullptr || m_windowStarted || (m_appLock != nullptr && !m_appLock->canStartSources()))
        return;
    // Starting while locked may resume saved pictures, but can never bypass setup/recovery.
    m_windowStarted = true;
    const auto source = m_selectedSource;
    if (source == u"pc"_s) {
        enterOwnWall();
    } else if (source != u"connect"_s) {
        takeTheServer();
        m_foregroundAddress = source;
        m_server.connectTo(source);
        rememberSource(source);
    }
    showSources();
}

ServerSession* ServerSession::observerAt(const QString& address) const
{
    for (const auto& child : m_windowSessions) {
        if (child != nullptr && !child->m_serverScope.isEmpty() && child->m_foregroundAddress == address)
            return child;
    }
    return nullptr;
}

bool ServerSession::observesServer() const
{
    return m_coordinator == nullptr || m_coordinator->observerAt(m_foregroundAddress) == this;
}

void ServerSession::useQuickAccess(QuickAccess& shortcuts)
{
    m_shortcuts = &shortcuts;
}

void ServerSession::useAlerts(AlertsView& alerts)
{
    m_alerts = &alerts;
    connect(&alerts, &AlertsView::changed, this, &ServerSession::showSources);
    const auto entered = [this, &alerts](const core::Account& account) {
        const auto scope = incidentScope(m_server.address(), account.id);
        if (m_serverScope != scope && !m_serverScope.isEmpty() && observesServer())
            alerts.suspend(m_serverScope);
        m_serverScope = scope;
        auto* coordinator = m_coordinator != nullptr ? m_coordinator : this;
        // A new account invalidates the old account's views immediately on this PC.
        for (const auto& other : coordinator->m_windowSessions) {
            if (other != nullptr && other != this && other->m_foregroundAddress == m_foregroundAddress &&
                !other->m_serverScope.isEmpty() && other->m_serverScope != scope) {
                other->m_server.leave();
            }
        }
        coordinator->rememberAlertAddress(scope, m_server.address().toString());
        if (observesServer()) {
            coordinator->stopBackgroundAt(m_foregroundAddress);
            alerts.watch(scope, false, m_server.address().authority());
            alerts.context(scope, false);
        }
    };
    connect(&m_server, &Source::entered, this, [this, entered](const core::Account& account) {
        entered(account);
        m_openConfirmed = true;
    });
    connect(&m_server, &Source::whoChanged, this, entered);
    connect(&m_server, &Source::left, this, [this, &alerts] {
        if (observesServer())
            alerts.suspend(m_serverScope);
        m_serverScope.clear();
        QMetaObject::invokeMethod(this, &ServerSession::refreshMonitoring, Qt::QueuedConnection);
    });
    connect(&m_server.feed(), &core::WallFeed::wallChanged, this, [this, &alerts] {
        if (m_server.isAtTheWall()) {
            if (observesServer())
                alerts.inventoryFor(m_serverScope, m_server.feed().machines());
            finishOpeningIncident();
        }
    });
    connect(&m_server.feed(), &core::WallFeed::statusObserved, this, [this, &alerts](bool contact) {
        if (m_server.isAtTheWall() && observesServer())
            alerts.observeFor(m_serverScope, m_server.feed().status(), contact);
    });
    if (m_pc) {
        if (m_coordinator == nullptr)
            m_pc->monitorInBackground();
        connect(m_pc.get(), &Source::entered, this, [this, &alerts] {
            if (m_coordinator == nullptr)
                alerts.watch(u"pc"_s, true, words("This PC"));
            alerts.context(u"pc"_s, true);
        });
        connect(&m_pc->feed(), &core::WallFeed::wallChanged, this, [this, &alerts] {
            if (m_coordinator == nullptr)
                alerts.inventoryFor(u"pc"_s, m_pc->feed().machines());
            if (onOwnWall())
                finishOpeningIncident();
        });
        connect(&m_pc->feed(), &core::WallFeed::statusObserved, this, [this, &alerts](bool contact) {
            if (m_coordinator == nullptr)
                alerts.observeFor(u"pc"_s, m_pc->feed().status(), contact);
        });
    }
    connect(&m_view, &SessionView::changed, this, &ServerSession::refreshMonitoring, Qt::QueuedConnection);
}

void ServerSession::rememberAlertAddress(const QString& scope, const QString& address)
{
    const auto previous = m_alertAddresses.keys(address);
    for (const auto& key : previous) {
        if (key == scope)
            continue;
        m_alertAddresses.remove(key);
        if (m_alerts != nullptr) {
            const bool selected = m_alerts->scope() == key;
            m_alerts->forgetSource(key);
            if (selected)
                m_alerts->context(scope, false);
        }
    }
    m_alertAddresses.insert(scope, address);
    showSources();
    for (const auto& child : m_windowSessions) {
        if (child != nullptr)
            child->showSources();
    }
}

void ServerSession::stopBackgroundAt(const QString& address)
{
    std::erase_if(m_background, [this, &address](const auto& background) {
        if (background->source->address().toString() != address)
            return false;
        if (m_alerts != nullptr) {
            m_alerts->suspend(background->scope);
            m_alerts->forgetSource(background->placeholder);
        }
        return true;
    });
}

void ServerSession::refreshMonitoring()
{
    if (m_coordinator != nullptr) {
        m_coordinator->refreshMonitoring();
        return;
    }
    for (const auto& child : m_windowSessions) {
        if (child != nullptr)
            child->showSources();
    }
    showSources();
    if (m_alerts == nullptr || m_monitoringStopped)
        return;
    if (m_screen.isOne() || (m_appLock != nullptr && (!m_appLock->canStartSources() || m_appLock->wallScreen()))) {
        for (const auto& background : m_background)
            m_alerts->suspend(background->scope);
        m_background.clear();
        if (m_pc && !onOwnWall()) {
            m_pc->feed().stop();
            m_alerts->suspend(u"pc"_s);
        }
        return;
    }
    if (m_pc && !m_pc->feed().isRunning()) {
        m_alerts->watch(u"pc"_s, true, words("This PC"));
        m_pc->feed().start();
    }
    auto addresses = m_settings.remembered().servers;
    // The last server from an earlier build is checked only if its session is still saved.
    if (const auto last = core::ServerClient::addressFrom(m_settings.remembered().server);
        last && !addresses.contains(last.value().toString()) &&
        !m_secrets.kept(u"session at "_s + last.value().toString()).isEmpty())
        addresses.append(last.value().toString());
    addresses = addresses.first(std::min(addresses.size(), core::Remembered::mostMonitoredServers));
    std::erase_if(m_background, [this, &addresses](const auto& background) {
        const auto address = background->source->address().toString();
        if (addresses.contains(address) && address != m_foregroundAddress && observerAt(address) == nullptr)
            return false;
        m_alerts->suspend(background->scope);
        m_alerts->forgetSource(background->placeholder);
        return true;
    });
    for (const auto& child : m_windowSessions) {
        if (child != nullptr && !child->m_serverScope.isEmpty() && child->observesServer()) {
            m_alerts->watch(child->m_serverScope, false, child->m_server.address().authority());
            m_alerts->inventoryFor(child->m_serverScope, child->m_server.feed().machines());
        }
    }
    for (const auto& address : addresses) {
        if (address == m_foregroundAddress || observerAt(address) != nullptr ||
            std::ranges::any_of(m_background, [&address](const auto& background) {
                return background->source->address().toString() == address;
            }))
            continue;
        auto background = std::make_unique<Background>();
        auto* watched = background.get();
        background->source = std::make_unique<ServerSource>(u"monitor"_s, m_settings, m_secrets, m_statusEveryMs);
        auto* source = background->source.get();
        background->placeholder = incidentScope(QUrl(address), {});
        m_alertAddresses.insert(background->placeholder, address);
        source->setBackground();
        connect(source, &ServerSource::changed, this, [this, watched, source] {
            if (m_alerts == nullptr)
                return;
            if (source->isAtTheWall()) {
                m_alerts->forgetSource(watched->placeholder);
                return;
            }
            const auto stage = source->shown().stage;
            const auto reason = stage == u"signIn"_s  ? words("Sign in again to resume monitoring")
                                : stage == u"trust"_s ? words("Review this server's certificate to resume monitoring")
                                                      : words("Connecting; open this source if it needs attention");
            m_alertAddresses.insert(watched->placeholder, source->address().toString());
            m_alerts->unavailable(watched->placeholder, source->address().authority(), reason);
        });
        const auto enter = [this, watched, source](const core::Account& account) {
            if (m_alerts == nullptr)
                return;
            const auto scope = incidentScope(source->address(), account.id);
            if (scope != watched->scope && !watched->scope.isEmpty())
                m_alerts->suspend(watched->scope);
            watched->scope = scope;
            rememberAlertAddress(scope, source->address().toString());
            m_alerts->watch(scope, false, source->address().authority());
        };
        connect(source, &Source::entered, this, enter);
        connect(source, &Source::whoChanged, this, enter);
        connect(source, &Source::left, this, [this, watched] {
            if (m_alerts != nullptr)
                m_alerts->suspend(watched->scope);
        });
        connect(&source->feed(), &core::WallFeed::wallChanged, this, [this, watched, source] {
            if (m_alerts != nullptr && source->isAtTheWall())
                m_alerts->inventoryFor(watched->scope, source->feed().machines());
        });
        connect(&source->feed(), &core::WallFeed::statusObserved, this, [this, watched, source](bool contact) {
            if (m_alerts != nullptr && source->isAtTheWall())
                m_alerts->observeFor(watched->scope, source->feed().status(), contact);
            if (contact)
                source->looked();
        });
        m_background.push_back(std::move(background));
        source->connectTo(address);
    }
}

void ServerSession::openIncident(const QString& scope, const QString& machine, WallView& wall)
{
    if (m_screen.isOne() || m_monitoringStopped)
        return;
    m_openWall = &wall;
    m_openScope = scope;
    m_openMachine = machine;
    m_openConfirmed = false;
    if (m_appLock != nullptr && m_appLock->restricted()) {
        m_appLock->ask(u"unlock"_s);
        return;
    }
    if (scope == u"pc"_s && m_pc) {
        m_openConfirmed = true;
        enterOwnWall();
        m_pc->feed().stop();
        m_pc->feed().start();
        return;
    }
    const auto address = (m_coordinator != nullptr ? m_coordinator : this)->m_alertAddresses.value(scope);
    if (address.isEmpty() || (!m_settings.remembered().servers.contains(address) && scope != m_serverScope &&
                              (m_coordinator == nullptr || m_coordinator->observerAt(address) == nullptr))) {
        m_openWall = nullptr;
        emit incidentUnavailable();
        return;
    }
    if (!onOwnWall() && m_serverScope == scope && m_server.isAtTheWall()) {
        m_server.verifyAccount([this, scope, target = m_openWall](const QString& account) {
            if (m_openWall != target || m_openScope != scope)
                return;
            if (account.isEmpty() || incidentScope(m_server.address(), account) != scope) {
                m_openWall = nullptr;
                emit incidentUnavailable();
                return;
            }
            m_openConfirmed = true;
            m_server.feed().stop();
            m_server.feed().start();
        });
        return;
    }
    leaveOwnWall();
    m_server.standAside();
    connectTo(address);
}

void ServerSession::finishOpeningIncident()
{
    if (m_openWall == nullptr || !m_openConfirmed || (m_appLock != nullptr && m_appLock->restricted()))
        return;
    const auto scope = onOwnWall() ? u"pc"_s : m_serverScope;
    if (scope.isEmpty())
        return;
    auto* wall = m_openWall.data();
    m_openWall = nullptr;
    if (scope != m_openScope && !(m_openMachine.isEmpty() && m_openScope == incidentScope(m_server.address(), {}))) {
        emit incidentUnavailable();
        return;
    }
    if (m_alerts != nullptr)
        m_alerts->choose(scope);
    if (m_openMachine.isEmpty() || m_openMachine == u"@source"_s) {
        emit incidentDetailsAsked();
        return;
    }
    const auto key = wallKey(onOwnWall() ? PcSource::idOfThisPc() : serverId(), m_openMachine);
    if (key) {
        wall->stopArranging();
        wall->enlarge(*key);
        if (wall->enlarged() == *key) {
            emit wall->revealAsked();
            return;
        }
    }
    emit incidentUnavailable();
}

void ServerSession::useAppLock(AppLock& lock)
{
    m_appLock = &lock;
    if (m_screen.isOne()) {
        lock.becameWallScreen();
    }
    m_merged.setAppRestricted(lock.restricted());
    connect(
        &lock, &AppLock::changed, this, [this, &lock] { m_merged.setAppRestricted(lock.restricted()); },
        Qt::DirectConnection);
    connect(
        &m_screen, &WallScreen::changed, &lock,
        [this, &lock] {
            if (m_screen.isOne()) {
                lock.becameWallScreen();
            }
        },
        Qt::DirectConnection);
    if (m_coordinator == nullptr)
        lock.endScreenWith([this] { return clearScreens(); });
    connect(&lock, &AppLock::resetStarting, this, &ServerSession::stopForReset);
    connect(&lock, &AppLock::changed, this, [this] {
        beginWindow();
        showSources();
        refreshMonitoring();
        if (m_appLock->restricted() && m_appLock->mode().isEmpty())
            m_openWall = nullptr;
        if (!m_appLock->restricted() && m_openWall != nullptr)
            openIncident(m_openScope, m_openMachine, *m_openWall);
    });
}

bool ServerSession::clearScreens()
{
    m_server.standAside();
    for (const auto& child : m_windowSessions) {
        if (child != nullptr) {
            child->m_server.standAside();
            if (!child->m_screen.clearLocally())
                return false;
            child->show();
        }
    }
    const bool cleared = static_cast<bool>(m_screen.clearLocally());
    show();
    return cleared;
}

void ServerSession::stopForReset()
{
    m_monitoringStopped = true;
    m_openWall = nullptr;
    m_background.clear();
    if (m_pc)
        m_pc->feed().stop();
    m_server.standAside();
    leaveOwnWall();
    emit wallWasLeft();
}

void ServerSession::takeTheServer()
{
    if (std::exchange(m_serverTaken, true)) {
        return;
    }
    // The server's wall is the windows' while somebody is at it. (First: the windows hear what it
    // says of itself before the session does, which is direct as well.)
    m_merged.addSource(m_server);
    connect(&m_server, &Source::entered, this, [this] { rememberWhichWall(false); }, Qt::DirectConnection);
    connect(&m_server, &Source::left, this, &ServerSession::wallWasLeft, Qt::DirectConnection);
}

ServerSession::~ServerSession()
{
    // Do not ask for pictures during teardown: some fixture views may already be gone.
    // These listeners otherwise outlive this session because their receiver is a window.
    for (const auto& wall : m_walls) {
        if (wall != nullptr) {
            if (m_shortcuts != nullptr)
                disconnect(m_shortcuts, nullptr, wall, nullptr);
            disconnect(&m_merged, nullptr, wall, nullptr);
            disconnect(wall, nullptr, this, nullptr);
        }
    }
    if (m_coordinator != nullptr && m_alerts != nullptr && observesServer())
        m_alerts->suspend(m_serverScope);
    m_serverScope.clear();
    // (What every window has of the wall, its streams and keyboard and mouse, goes with the
    // windows' wall, which ends first.)
    m_server.feed().stop();
    if (m_pc != nullptr) {
        m_pc->feed().stop();
    }
}

QString ServerSession::serverId()
{
    return u"server"_s;
}

bool ServerSession::onOwnWall() const
{
    return m_pc != nullptr && m_pc->isAtTheWall();
}

bool ServerSession::isAtAServersWall() const
{
    return !onOwnWall() && m_server.isAtTheWall();
}

void ServerSession::useOwnWall(core::LocalWall& wall)
{
    if (m_pc != nullptr) {
        return;
    }
    // What changes on that wall while it is shown goes to its feed at once (PcSource). Its
    // machines are the windows' while its wall is shown, before the server's (which come after
    // them from here on).
    m_localWall = &wall;
    m_pc = std::make_unique<PcSource>(wall, m_statusEveryMs);
    m_merged.addSource(*m_pc);
    takeTheServer();
    // (Whether everything on the wall is spoken to encrypted may be otherwise now.)
    connect(m_pc.get(), &PcSource::kvmsChanged, this, &ServerSession::show);
    // Whatever a box of a file has under way when this wall is left is nobody's, as at a server's.
    connect(m_pc.get(), &Source::left, this, &ServerSession::wallWasLeft, Qt::DirectConnection);
}

void ServerSession::rememberWhichWall(bool ownWall)
{
    if (m_coordinator != nullptr)
        return;
    core::Remembered remembered = m_settings.remembered();
    if (remembered.showsOwnWall != ownWall) {
        remembered.showsOwnWall = ownWall;
        // (A PC whose settings cannot be written starts into the other wall the next time.)
        static_cast<void>(m_settings.remember(remembered));
    }
}

void ServerSession::enterOwnWall()
{
    if (m_appLock != nullptr && !m_appLock->canStartSources()) {
        return;
    }
    if (m_pc == nullptr)
        return;
    rememberSource(u"pc"_s);
    if (onOwnWall())
        return;
    // Away from a server's wall first, and from whatever was being asked of a server: signed in
    // there or not, that is left as it is (a session that was kept stays kept). Its feed has
    // stopped with that, or never ran.
    m_foregroundAddress.clear();
    m_server.standAside();
    // Whoever sits at this PC may do everything on its own wall: there are no accounts on it. It
    // says so itself to the windows' wall.
    m_pc->enter();
    rememberWhichWall(true);
    show();
    if (m_pc->feed().isRunning())
        m_pc->feed().askAnew();
    else
        m_pc->feed().start();
}

void ServerSession::leaveOwnWall()
{
    if (!onOwnWall()) {
        return;
    }
    // The displayed wall is left. Its status feed stays alive when monitoring is attached;
    // the windows give up pictures and controls through Source::left and Source::emptied.
    if (m_alerts == nullptr || m_monitoringStopped || m_coordinator != nullptr)
        m_pc->feed().stop();
    m_pc->leave();
    show();
}

void ServerSession::add(WallView& wall, ActionsView& actions, VideoHost* host)
{
    if (m_walls.contains(&wall))
        return;
    m_walls.append(&wall);
    m_merged.add(wall, actions, [shown = QPointer<VideoHost>(host)]() -> engine::Scene* {
        return shown != nullptr ? &shown->scene() : nullptr;
    });
    // Each machine shown large at the size last chosen for it (b10), and the size chosen kept.
    connect(&wall, &WallView::enlargedChanged, this,
            [this, &wall] { wall.setEnlargedScale(scaleKeptFor(wall.enlarged())); });
    connect(&wall, &WallView::scaleChosen, this,
            [this](const QString& key, const QString& choice) { keepScale(key, choice); });
    if (m_shortcuts != nullptr) {
        connect(m_shortcuts, &QuickAccess::changed, &wall, [this, &wall] { showFavorites(wall); });
        connect(&m_merged, &WallMachines::wallChanged, &wall, [this, &wall] { showFavorites(wall); });
        connect(&wall, &WallView::favoriteAsked, this, [this, &wall](const QString& key) {
            if (!wall.shortcutsAvailable())
                return;
            if (const auto reference = shortcutOf(key))
                m_shortcuts->toggleFavorite(*reference);
        });
        connect(&wall, &WallView::enlargedChanged, this, [this, &wall] {
            if (const auto reference = shortcutOf(wall.enlarged()))
                m_shortcuts->opened(*reference);
        });
        showFavorites(wall);
    }
}

std::optional<core::MachineReference> ServerSession::shortcutOf(const QString& key) const
{
    if (m_screen.isOne() || m_monitoringStopped || (m_appLock != nullptr && m_appLock->restricted()))
        return std::nullopt;
    const auto reached = m_merged.reach(key);
    if (!reached)
        return std::nullopt;
    const auto scope = reached.source == PcSource::idOfThisPc() ? u"pc"_s : m_serverScope;
    if (scope.isEmpty())
        return std::nullopt;
    return core::MachineReference{scope, reached.machine};
}

std::optional<core::MachineReference> ServerSession::scaleReferenceOf(const QString& key) const
{
    const auto reached = m_merged.reach(key);
    if (!reached) {
        return std::nullopt;
    }
    const auto scope = reached.source == PcSource::idOfThisPc() ? u"pc"_s : m_scaleScope;
    if (scope.isEmpty()) {
        return std::nullopt;
    }
    return core::MachineReference{scope, reached.machine};
}

QString ServerSession::scaleKeptFor(const QString& key) const
{
    const auto reference = scaleReferenceOf(key);
    if (!reference) {
        return u"fit"_s;
    }
    const auto& kept = m_settings.remembered().pictureScales;
    const auto found =
        std::ranges::find_if(kept, [&](const core::MachineScale& one) { return one.machine == *reference; });
    return found == kept.end() ? u"fit"_s : found->scale;
}

void ServerSession::keepScale(const QString& key, const QString& choice)
{
    const auto reference = scaleReferenceOf(key);
    if (!reference) {
        return;
    }
    // The one chosen last first, once for each machine, and no more than are kept; "fit" keeps nothing.
    core::Remembered remembered = m_settings.remembered();
    auto& kept = remembered.pictureScales;
    kept.removeIf([&](const core::MachineScale& one) { return one.machine == *reference; });
    if (choice != u"fit"_s) {
        kept.prepend({*reference, choice});
    }
    if (kept.size() > core::Remembered::mostPictureScales) {
        kept.resize(core::Remembered::mostPictureScales);
    }
    if (kept != m_settings.remembered().pictureScales) {
        static_cast<void>(m_settings.remember(remembered));
    }
}

void ServerSession::showFavorites(WallView& wall)
{
    QStringList keys;
    if (m_shortcuts != nullptr) {
        for (const auto& machine : m_merged.machines()) {
            if (const auto reference = shortcutOf(machine.id); reference && m_shortcuts->isFavorite(*reference))
                keys.append(machine.id);
        }
    }
    wall.setFavorites(keys,
                      m_shortcuts != nullptr && m_shortcuts->allowed() && !m_screen.isOne() && !m_monitoringStopped);
}

void ServerSession::remove(WallView& wall)
{
    m_walls.removeAll(&wall);
    if (m_shortcuts != nullptr) {
        disconnect(m_shortcuts, nullptr, &wall, nullptr);
        disconnect(&m_merged, nullptr, &wall, nullptr);
        disconnect(&wall, nullptr, this, nullptr);
        wall.setFavorites({}, false);
    }
    m_merged.remove(wall);
}

void ServerSession::drive(WallView& wall, MachineControl& control)
{
    m_merged.drive(wall, control);
}

void ServerSession::serve(FilesView& files)
{
    if (m_appLock != nullptr) {
        connect(&files, &FilesView::changed, this, [this, &files] {
            if (!files.isBusy()) {
                m_appLock->cancelApproval(&files);
            }
        });
        connect(m_appLock, &AppLock::approvalCancelled, &files, [&files](QObject* owner) {
            if (owner == &files) {
                files.reset();
            }
        });
        connect(&files, &QObject::destroyed, this, [this, owner = &files] { m_appLock->cancelApproval(owner); });
    }
    // Whatever a box of a file has under way, or holds, when the wall is left is nobody's; and so
    // it is where the program comes to only show.
    connect(this, &ServerSession::wallWasLeft, &files, &FilesView::reset);
    connect(&m_merged, &MergedWall::onlyShowsChanged, &files, [this, asked = QPointer<FilesView>(&files)] {
        if (m_merged.onlyShows() && asked != nullptr) {
            asked->reset();
        }
    });
    // What it asks is asked of the server (ServerSource::exportWall, readFile, takeFile), with the
    // box: each answer is told under the number it was asked under, and the box knows whether it
    // still waits for it. Whether it is sent at all is the server's conversation's to say; and
    // nothing is, from a program that only shows.
    const QPointer<FilesView> asked(&files);
    const auto onlyShown = [] {
        return base::Error{
            FilesView::nothingWasSent,
            words("This PC only shows the wall: nothing is done from it. Nothing was sent.").toStdString()};
    };
    connect(&files, &FilesView::exportAsked, this,
            [this, asked, onlyShown](quint64 turn, const QStringList& parts, const QString& passphrase,
                                     const QString& password) {
                if (m_merged.onlyShows()) {
                    asked->exported(turn, onlyShown());
                    return;
                }
                if (m_appLock != nullptr && !passphrase.isEmpty()) {
                    const auto phrase = std::make_shared<ExportPassword>(passphrase);
                    const auto accountPassword = std::make_shared<ExportPassword>(password);
                    if (!m_appLock->authorizeExport(asked, [this, asked, turn, parts, phrase, accountPassword] {
                            if (asked != nullptr && asked->awaitsExport(turn) && !m_merged.onlyShows()) {
                                m_server.exportWall(asked, turn, parts, phrase->text, accountPassword->text);
                            }
                        })) {
                        asked->exported(turn, base::Error{FilesView::nothingWasSent,
                                                          "Finish the other app password request first."});
                    }
                    return;
                }
                m_server.exportWall(asked, turn, parts, passphrase, password);
            });
    connect(&files, &FilesView::readAsked, this,
            [this, asked, onlyShown](quint64 turn, const QByteArray& file, const QString& passphrase) {
                if (m_merged.onlyShows()) {
                    asked->fileWasRead(turn, onlyShown());
                    return;
                }
                m_server.readFile(asked, turn, file, passphrase);
            });
    connect(&files, &FilesView::takeAsked, this,
            [this, asked, onlyShown](quint64 turn, const QByteArray& file, const QString& passphrase,
                                     const QStringList& parts) {
                if (m_merged.onlyShows()) {
                    asked->fileWasTaken(turn, onlyShown());
                    return;
                }
                m_server.takeFile(asked, turn, file, passphrase, parts);
            });
}

void ServerSession::serve(EditKvmView& editing)
{
    editing.setMayChange(!m_merged.onlyShows());
    connect(&m_merged, &MergedWall::onlyShowsChanged, &editing,
            [this, &editing] { editing.setMayChange(!m_merged.onlyShows()); });
}

void ServerSession::openPagesWith(std::function<bool(const QUrl&)> opener)
{
    m_opener = opener;
    m_merged.openPagesWith(std::move(opener));
}

void ServerSession::tellTheTimeWith(std::function<QDateTime()> clock)
{
    m_server.tellTheTimeWith(std::move(clock));
}

void ServerSession::connectTo(const QString& address)
{
    if (m_appLock != nullptr && m_appLock->restricted()) {
        return;
    }
    // (A server is named from where one is asked for, and not from this PC's own wall: that is
    // left first.)
    if (onOwnWall()) {
        return;
    }
    takeTheServer();
    if (const auto parsed = core::ServerClient::addressFrom(address); parsed) {
        m_foregroundAddress = parsed.value().toString();
        rememberSource(m_foregroundAddress);
        if (m_coordinator == nullptr)
            stopBackgroundAt(m_foregroundAddress);
    }
    m_server.connectTo(address);
}

void ServerSession::show()
{
    showSources();
    // An order made on the wall is made of the machines of the wall that is shown.
    m_merged.setOrderOf(onOwnWall() ? m_pc->id() : m_server.id());
    if (!onOwnWall()) {
        m_view.show(m_server.shown());
        return;
    }
    // This PC's own wall: no server and nobody signed in, and nothing travels to one.
    SessionView::Shown shown;
    shown.stage = u"wall"_s;
    shown.ownWall = true;
    shown.manages = true;
    // Nothing travels to a server. What travels to a KVM does so as that KVM was added.
    shown.encrypted = m_pc->isEncrypted();
    if (const base::Result<QUrl> last = core::ServerClient::addressFrom(m_settings.remembered().server)) {
        shown.suggested = core::ServerClient::asTyped(last.value());
    }
    m_view.show(shown);
}

void ServerSession::begin()
{
    if (m_appLock != nullptr && !m_appLock->canStartSources())
        return;
    m_monitoringStopped = false;
    // Earlier builds kept each server's session but only the last address in settings.
    // Recover the saved addresses from credential names; secret values stay in their store.
    if (m_alerts != nullptr && !m_screen.isOne()) {
        auto saved = m_settings.remembered();
        if (const auto names = m_secrets.names(); names) {
            for (const auto& name : names.value()) {
                if (!name.startsWith(u"session at "_s) ||
                    saved.servers.size() >= core::Remembered::mostMonitoredServers)
                    continue;
                const auto address = core::ServerClient::addressFrom(name.mid(11));
                if (address && name == u"session at "_s + address.value().toString() &&
                    !saved.servers.contains(address.value().toString()))
                    saved.servers.append(address.value().toString());
            }
        }
        if (saved != m_settings.remembered())
            static_cast<void>(m_settings.remember(saved));
    }
    const core::Remembered& remembered = m_settings.remembered();
    if (m_pc != nullptr && (m_windowMode || remembered.showsOwnWall || remembered.server.isEmpty())) {
        enterOwnWall();
        refreshMonitoring();
        return;
    }
    if (!remembered.server.isEmpty()) {
        connectTo(remembered.server);
    }
    refreshMonitoring();
}

} // namespace qkvm::ui
