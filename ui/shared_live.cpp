#include "shared_live.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <QTimer>

using namespace Qt::StringLiterals;

namespace qkvm::ui {
class SharedLiveHandle;

class SharedLiveGroup final : public QObject {
public:
    SharedLiveGroup(int releaseWithinMs, QObject* parent) : QObject(parent)
    {
        releaseDeadline.setSingleShot(true);
        releaseDeadline.setInterval(releaseWithinMs);
        connect(&releaseDeadline, &QTimer::timeout, this, [this] {
            end(u"desktop:control_unconfirmed"_s, tr("Control release was not confirmed. Reconnecting live video."));
        });
    }
    ~SharedLiveGroup() override
    {
        if (line != nullptr)
            line->close();
    }
    void start(core::WallSource& source, QObject& lifetime, const QString& machine);
    void attach(SharedLiveHandle& handle);
    void detach(SharedLiveHandle& handle);
    void resize();
    void control(SharedLiveHandle& handle, bool on, const QString& takeover = {});
    void tellOwner();
    void end(const QString& code, const QString& message);
    void retire();
    core::Live::Need need() const;
    template <typename F>
    void each(F action);

    std::unique_ptr<core::Live> line;
    QList<QPointer<SharedLiveHandle>> handles;
    QPointer<SharedLiveHandle> controller;
    QPointer<SharedLiveHandle> lastController;
    std::optional<core::Live::Video> video;
    std::function<void()> forgotten;
    bool finished = false;
    bool releasing = false;
    QList<bool> requests;
    QTimer releaseDeadline;
};

class SharedLiveHandle final : public core::Live {
public:
    SharedLiveHandle(SharedLiveGroup& group, Need need) : group(&group), wanted(need) { group.attach(*this); }
    ~SharedLiveHandle() override { close(); }
    bool isOpen() const override { return group != nullptr && group->line != nullptr && group->line->isOpen(); }
    Pointer pointerMode() const override { return isOpen() ? group->line->pointerMode() : Pointer::Unknown; }
    QString transport() const override { return isOpen() ? group->line->transport() : QString(); }
    QString fallbackReason() const override { return isOpen() ? group->line->fallbackReason() : QString(); }
    // What the shared line let go since this one joined it, and never less than said before, also once
    // the line is closed (2c, the second reading).
    quint64 picturesLetGo() const override
    {
        if (isOpen()) {
            const quint64 now = group->line->picturesLetGo();
            letGoSaid = std::max(letGoSaid, now > letGoAtJoin ? now - letGoAtJoin : 0);
        }
        return letGoSaid;
    }
    void videoFailed() override
    {
        if (isOpen())
            group->line->videoFailed();
    }
    // As the shared line said when it ended (the group is gone by the time anybody asks).
    bool endsForGood(const QString& code) const override { return code == endedWith && endedForGood; }
    void setNeed(Need need) override
    {
        wanted = need;
        if (group != nullptr)
            group->resize();
    }
    void askForAKeyFrame() override
    {
        waitingForKey = true;
        if (isOpen())
            group->line->askForAKeyFrame();
    }
    void askControl(bool on) override
    {
        if (group != nullptr)
            group->control(*this, on);
    }
    void chooseRelativeMouse(bool relative) override { relativeMouse = relative; }
    ControlOwner controlOwner() const override
    {
        if (!isOpen())
            return {};
        if (group->controller != nullptr && group->controller != this)
            return {{}, {}, true};
        return group->line->controlOwner();
    }
    void takeOver(const QString& expectedOwner) override
    {
        if (group != nullptr && !expectedOwner.isEmpty())
            group->control(*this, true, expectedOwner);
    }
    bool hasControl() const override
    {
        return isOpen() && group->controller == this && !group->releasing && group->line->hasControl();
    }
    void key(int usage, bool down) override
    {
        if (hasControl())
            group->line->key(usage, down);
    }
    void pointerAt(int x, int y) override
    {
        if (hasControl())
            group->line->pointerAt(x, y);
    }
    void pointerBy(int dx, int dy) override
    {
        if (hasControl())
            group->line->pointerBy(dx, dy);
    }
    void button(int number, bool down) override
    {
        if (hasControl())
            group->line->button(number, down);
    }
    void wheel(int dx, int dy) override
    {
        if (hasControl())
            group->line->wheel(dx, dy);
    }
    void releaseAll() override
    {
        if (hasControl())
            group->line->releaseAll();
    }
    void close() override
    {
        if (group != nullptr) {
            auto* previous = group.data();
            previous->detach(*this);
            group = nullptr;
        }
    }
    QPointer<SharedLiveGroup> group;
    Need wanted;
    bool ready = false;
    bool waitingForKey = true;
    // How the shared line ended, for endsForGood().
    QString endedWith;
    bool endedForGood = false;
    bool relativeMouse = false;
    // What the shared line had let go as this one joined it, and the most said since.
    quint64 letGoAtJoin = 0;
    mutable quint64 letGoSaid = 0;
};

template <typename F>
void SharedLiveGroup::each(F action)
{
    const auto copy = handles;
    for (const auto& handle : copy) {
        if (handle != nullptr && handle->group == this && handle->ready)
            action(*handle);
    }
}

core::Live::Need SharedLiveGroup::need() const
{
    core::Live::Need result{1, 1};
    for (const auto& handle : handles) {
        if (handle != nullptr) {
            result.maxWidth = std::max(result.maxWidth, handle->wanted.maxWidth);
            result.maxFps = std::max(result.maxFps, handle->wanted.maxFps);
        }
    }
    return result;
}

void SharedLiveGroup::resize()
{
    if (line != nullptr && !finished)
        line->setNeed(need());
}

void SharedLiveGroup::attach(SharedLiveHandle& handle)
{
    handles.append(&handle);
    handle.letGoAtJoin = line != nullptr ? line->picturesLetGo() : 0;
    // Delivery to the caller is queued before this, so no signal precedes handover.
    QTimer::singleShot(0, &handle, [this, &handle] {
        if (handle.group != this || finished)
            return;
        handle.ready = true;
        if (line != nullptr) {
            if (video)
                emit handle.videoChanged(*video);
            if (handle.isOpen())
                emit handle.opened();
            if (handle.group == this) {
                resize();
                line->askForAKeyFrame();
            }
        }
    });
}

void SharedLiveGroup::tellOwner()
{
    each([](SharedLiveHandle& handle) { emit handle.controlOwnerChanged(); });
}

void SharedLiveGroup::control(SharedLiveHandle& handle, bool on, const QString& takeover)
{
    if (line == nullptr || finished)
        return;
    if (on) {
        if ((controller != nullptr && controller != &handle) || releasing) {
            emit handle.controlChanged(false, u"desktop:control_busy"_s,
                                       tr("Keyboard and mouse are in use in another window."));
            return;
        }
        if (!takeover.isEmpty() && takeover != line->controlOwner().reference) {
            emit handle.controlChanged(false, u"desktop:owner_changed"_s,
                                       tr("The control holder changed. Ask for control again."));
            return;
        }
        controller = &handle;
        lastController = &handle;
        line->chooseRelativeMouse(handle.relativeMouse);
        requests.append(true);
        if (takeover.isEmpty())
            line->askControl(true);
        else
            line->takeOver(takeover);
        tellOwner();
    } else if (controller == &handle) {
        line->releaseAll();
        if (finished)
            return;
        releasing = true;
        requests.append(false);
        if (!releaseDeadline.isActive())
            releaseDeadline.start();
        line->askControl(false);
    } else {
        emit handle.controlChanged(false, {}, {});
    }
}

void SharedLiveGroup::detach(SharedLiveHandle& handle)
{
    // A release can synchronously end the line. The departing handle must hear nothing.
    handles.removeAll(&handle);
    if (controller == &handle && !finished)
        control(handle, false);
    if (handles.isEmpty())
        retire();
    else
        resize();
}

void SharedLiveGroup::retire()
{
    if (finished)
        return;
    finished = true;
    releaseDeadline.stop();
    if (forgotten)
        forgotten();
    if (line != nullptr)
        line->close();
    deleteLater();
}

void SharedLiveGroup::end(const QString& code, const QString& message)
{
    if (finished)
        return;
    const bool forGood = line != nullptr && line->endsForGood(code);
    retire();
    const auto copy = std::exchange(handles, {});
    for (const auto& handle : copy) {
        if (handle == nullptr)
            continue;
        handle->group = nullptr;
        handle->endedWith = code;
        handle->endedForGood = forGood;
        if (handle->ready) {
            emit handle->closed(code, message);
        } else {
            // A join queued just before the failure still gets its terminal event after handover.
            QTimer::singleShot(0, handle, [handle, code, message] { emit handle->closed(code, message); });
        }
    }
}

void SharedLiveGroup::start(core::WallSource& source, QObject& lifetime, const QString& machine)
{
    connect(&lifetime, &QObject::destroyed, this, [this] {
        if (line == nullptr)
            end(u"desktop:source_closed"_s, tr("The source was closed while connecting."));
    });
    source.openLive(machine, need(), [self = QPointer<SharedLiveGroup>(this)](auto result) {
        if (self == nullptr || self->finished)
            return;
        if (!result) {
            self->end(QString::fromStdString(result.error().code), QString::fromStdString(result.error().message));
            return;
        }
        self->line = std::move(result).value();
        auto* live = self->line.get();
        connect(live, &core::Live::opened, self,
                [self] { self->each([](SharedLiveHandle& handle) { emit handle.opened(); }); });
        connect(live, &core::Live::transportChanged, self,
                [self] { self->each([](SharedLiveHandle& handle) { emit handle.transportChanged(); }); });
        connect(live, &core::Live::pointerModeChanged, self,
                [self] { self->each([](SharedLiveHandle& handle) { emit handle.pointerModeChanged(); }); });
        connect(live, &core::Live::videoChanged, self, [self](const core::Live::Video& video) {
            self->video = video;
            self->each([&](SharedLiveHandle& handle) {
                handle.waitingForKey = true;
                emit handle.videoChanged(video);
            });
        });
        connect(live, &core::Live::pictureCame, self, [self](const core::Live::Picture& picture) {
            self->each([&](SharedLiveHandle& handle) {
                if (picture.h264 && handle.waitingForKey && !picture.keyFrame)
                    return;
                handle.waitingForKey = false;
                emit handle.pictureCame(picture);
            });
        });
        connect(live, &core::Live::stateChanged, self,
                [self](const QString& state, const QString& code, const QString& message) {
                    self->each([&](SharedLiveHandle& handle) { emit handle.stateChanged(state, code, message); });
                });
        connect(live, &core::Live::controlChanged, self, [self](bool on, const QString& code, const QString& message) {
            if (self->finished)
                return;
            // Do not reuse a controller until every earlier request has an answer. Servers
            // that omit an answer cause a bounded reconnect, never an ambiguous handoff.
            const bool answered = !self->requests.isEmpty();
            const bool askedOn = answered && self->requests.takeFirst();
            if (on && (!askedOn || self->controller == nullptr)) {
                if (askedOn && self->releasing) {
                    self->line->releaseAll();
                    return; // Its already-issued release is still awaiting an answer.
                }
                self->end(u"desktop:control_unconfirmed"_s,
                          tr("An unexpected control reply arrived. Reconnecting live video."));
                return;
            }
            const auto owner = self->controller;
            if (!on && self->requests.isEmpty()) {
                self->controller = nullptr;
                self->releasing = false;
                self->releaseDeadline.stop();
            }
            if (on && self->releasing)
                self->line->releaseAll();
            if (owner != nullptr && owner->group == self)
                emit owner->controlChanged(on, code, message);
            self->tellOwner();
        });
        connect(live, &core::Live::controlOwnerChanged, self, [self] { self->tellOwner(); });
        connect(live, &core::Live::leftDown, self, [self] {
            if (self->lastController != nullptr)
                emit self->lastController->leftDown();
        });
        connect(live, &core::Live::closed, self,
                [self](const QString& code, const QString& message) { self->end(code, message); });
        self->resize();
    });
}

SharedLive::SharedLive(int releaseWithinMs, QObject* parent) : QObject(parent), m_releaseWithinMs(releaseWithinMs) {}
SharedLive::~SharedLive()
{
    // End children while the registry still exists; their handles may outlive the pool.
    const auto groups = m_groups;
    for (const auto& group : groups) {
        if (group != nullptr)
            group->end(u"desktop:source_closed"_s, tr("The source was closed."));
    }
}

void SharedLive::open(const QString& identity, core::WallSource& source, QObject& lifetime, const QString& machine,
                      core::Live::Need need, Done done)
{
    // Length-prefix the identity so machine names cannot collide with another source.
    const auto key = QString::number(identity.size()) + u':' + identity + machine;
    auto group = m_groups.value(key);
    const bool created = group == nullptr;
    if (created) {
        auto added = std::make_unique<SharedLiveGroup>(m_releaseWithinMs, this);
        group = added.release(); // QObject parenting owns it until deferred retirement.
        m_groups.insert(key, group);
        group->forgotten = [this, key] { m_groups.remove(key); };
    }
    // Queue delivery before the handle's queued initialization.
    auto delivery = std::make_shared<std::unique_ptr<core::Live>>();
    QTimer::singleShot(0, this, [delivery, done = std::move(done)]() mutable { done(std::move(*delivery)); });
    *delivery = std::make_unique<SharedLiveHandle>(*group, need);
    if (created) {
        QTimer::singleShot(0, group, [group, &source, life = QPointer<QObject>(&lifetime), machine] {
            if (life != nullptr && !group->finished)
                group->start(source, *life, machine);
            else if (!group->finished)
                group->end(u"desktop:source_closed"_s, tr("The source was closed."));
        });
    }
}
} // namespace qkvm::ui
