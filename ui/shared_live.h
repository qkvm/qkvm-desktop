#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

#include <qkvm/core/wall_source.h>

namespace qkvm::ui {
class SharedLiveGroup;

// One wire connection per authenticated source and machine, with a separate Live for each
// window. Decoding stays in that window's renderer; keyboard and mouse have one owner.
class SharedLive final : public QObject {
public:
    using Done = std::function<void(base::Result<std::unique_ptr<core::Live>>)>;
    explicit SharedLive(int releaseWithinMs = 5000, QObject* parent = nullptr);
    ~SharedLive() override;
    void open(const QString& identity, core::WallSource& source, QObject& lifetime, const QString& machine,
              core::Live::Need need, Done done);

private:
    QHash<QString, QPointer<SharedLiveGroup>> m_groups;
    int m_releaseWithinMs;
};
} // namespace qkvm::ui
