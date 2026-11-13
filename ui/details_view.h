#pragma once

#include <QImage>
#include <QList>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <qkvm/core/screen_watch.h>
#include <qkvm/core/server_client.h>

namespace qkvm::ui {

// Per-window, memory-only history. A new source/account/permission context
// invalidates every old result. At most one bounded batch is in flight.
class DetailsView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The console owns its Details view.")
    Q_PROPERTY(bool opened READ opened WRITE setOpened NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool administrator READ administrator NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY changed)
    Q_PROPERTY(QVariantList graphs READ graphs NOTIFY changed)
    Q_PROPERTY(QVariantList activity READ activity NOTIFY changed)
    Q_PROPERTY(QString historyProblem READ historyProblem NOTIFY changed)
    Q_PROPERTY(QString graphProblem READ graphProblem NOTIFY changed)
    Q_PROPERTY(QString activityProblem READ activityProblem NOTIFY changed)
    Q_PROPERTY(QString graphNote READ graphNote NOTIFY changed)
    Q_PROPERTY(QString activityNote READ activityNote NOTIFY changed)
    // The picture that set off an alert of the machine's screen watch (b7), for an admin, asked for
    // only when it is looked at (showPicture): {open, title, at, state, problem}, its `state` being
    // "loading", "shown", "gone" (the server keeps it no more) or "failed" (`problem` says why).
    Q_PROPERTY(QVariantMap picture READ picture NOTIFY pictureChanged)
    Q_PROPERTY(QImage pictureImage READ pictureImage NOTIFY pictureChanged)
    // A KVM of this PC's own wall (bindOwn): no server's history, and its screen watches' alerts for activity.
    Q_PROPERTY(bool own READ own NOTIFY changed)

public:
    explicit DetailsView(QObject* parent = nullptr);
    void bind(core::ServerClient* client, const QString& machine, const QString& scope, bool administrator,
              bool readable);
    // A KVM of this PC's own wall, by its screen watches (c7): what they told of it is its activity, and the pictures
    // they kept are shown as a server's are (b7). Whoever sits here is its admin. None: bound to nothing.
    void bindOwn(core::ScreenWatch* screens, const QString& machine);
    bool own() const { return m_own != nullptr && available(); }
    bool opened() const { return m_opened; }
    void setOpened(bool opened);
    bool available() const { return (m_client != nullptr || m_own != nullptr) && m_readable && !m_machine.isEmpty(); }
    bool administrator() const { return m_administrator && available(); }
    bool busy() const { return m_opened && available() && (m_pending != 0 || m_again); }
    QVariantMap summary() const { return m_summary; }
    QVariantList graphs() const { return m_graphs; }
    QVariantList activity() const { return m_activity; }
    QString historyProblem() const { return m_historyProblem; }
    QString graphProblem() const { return m_graphProblem; }
    QString activityProblem() const { return m_activityProblem; }
    QString graphNote() const { return m_graphNote; }
    QString activityNote() const { return m_activityNote; }
    int refreshAfterMs() const { return m_refresh.interval(); }
    Q_INVOKABLE void refresh();
    QVariantMap picture() const { return m_picture; }
    QImage pictureImage() const { return m_pictureImage; }
    // The picture of the activity row `row` (its `picture` says it has one), asked of the server now;
    // and that it is looked at no more. Only for an admin, and for the machine the view is bound to.
    Q_INVOKABLE void showPicture(int row);
    Q_INVOKABLE void closePicture();
    // What a picture's answer is taken from: a JPEG or a PNG of no more than a monitor's worth of
    // pixels (`mostPixels`, and no side longer than 16384), judged by what it says of itself before
    // any of it is decoded; nothing for anything else.
    static constexpr qint64 mostPicturePixels = 40'000'000;
    // A machine's state in a word, as the page's LABELS (words.json, "states") say it, where an alert says what it
    // went from and to (held to words.json by test_ui_words); one added since as it came.
    static QString stateWord(const QString& state);
    static QImage pictureFrom(const QByteArray& bytes, qint64 mostPixels = mostPicturePixels);

signals:
    void changed();
    void pictureChanged();

private:
    bool accepts(quint64 epoch) const;
    void clear();
    void finish(quint64 epoch);
    bool denied(const base::Error& error);
    void show(const core::HistorySummary& summary);
    void show(const core::HistorySamples& samples);
    void show(const core::DeviceActivity& activity);
    static QByteArray identity(const core::ServerClient* client);

    QPointer<core::ServerClient> m_client;
    QPointer<core::ScreenWatch> m_own;
    QMetaObject::Connection m_ownSaid;
    QMetaObject::Connection m_ownRead; // (what stands there read back after it was bound: shown then)
    // What the own wall's screen watches told of the machine, as activity.
    void showOwn();
    // The picture's bytes came (or did not: gone), for turn `turn`.
    void pictureBytes(quint64 turn, const base::Result<QByteArray>& answer);
    QString m_machine;
    QString m_scope;
    QByteArray m_identity;
    bool m_administrator = false;
    bool m_readable = false;
    bool m_opened = false;
    bool m_again = false;
    bool m_retry = false;
    int m_pending = 0;
    int m_failures = 0;
    quint64 m_epoch = 0;
    quint64 m_batch = 0;
    QTimer m_refresh;
    QMetaObject::Connection m_batchGone;
    QVariantMap m_summary;
    QVariantList m_graphs;
    QVariantList m_activity;
    QString m_historyProblem;
    QString m_graphProblem;
    QString m_activityProblem;
    QString m_graphNote;
    QString m_activityNote;
    // Of each activity row: the stamp of its alert's picture (empty: none), what it is called, and
    // when the wall saw it.
    struct PictureOf {
        QString stamp;
        QString title;
        QString at;
    };
    QList<PictureOf> m_pictures;
    QVariantMap m_picture;
    QImage m_pictureImage;
    // Raised whenever another picture is looked at, or none: an answer of before is nobody's.
    quint64 m_pictureTurn = 0;
    void pictureCame(quint64 turn, const QImage& image);
};
} // namespace qkvm::ui
