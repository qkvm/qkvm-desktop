#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace qkvm::ui {

// What the program tells whoever sits at it, in a corner of the window, as the page's notices do:
// that something was done, that it was refused and why, or that something is under way. A notice
// goes by itself after a while; one that says something is under way stays until that is over.
// On the interface thread.
class Notices : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed their notices by the program.")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        TextRole = Qt::UserRole + 1,
        KindRole, // the page's word for it: "info", "ok", "error" or "working"
        NumberRole,
    };
    // Something worth knowing; something was done; something was not, or nobody knows whether it
    // was; something is under way.
    enum class Kind { Info, Done, Failed, UnderWay };

    // A test lets its notices go sooner: after `stayPercent` of the time they stay.
    explicit Notices(QObject* parent = nullptr, int stayPercent = 100);

    void tell(Kind kind, const QString& text);
    // Tells what was done while nobody may have been looking, and is said nowhere else. Where
    // nobody sees the window (setSeen) it stays until somebody does, and has its time from then:
    // told to a window behind the program's icon, it would have come and gone unread.
    void tellWhenSeen(Kind kind, const QString& text);
    // Whether anybody can see the window these are told in. Somebody can, until this is told
    // otherwise.
    void setSeen(bool seen);
    bool isSeen() const { return m_seen; }
    // What was told for when the window is seen, and waits for that still: each as it was told.
    struct Told {
        Kind kind = Kind::Info;
        QString text;
    };
    // Takes out what waits still, in the order it was told: for whoever tells it somewhere
    // else, where this window goes for good and nobody will see it again.
    QList<Told> takeWhatWaits();
    // Says that something is under way, until over() with the number this gives.
    int underWay(const QString& text);
    void over(int number);
    // All of them go: what they were about is nobody's any more.
    void clear();
    int count() const { return static_cast<int>(m_notices.size()); }

    // How long a notice stays, in milliseconds, as the page has it: long enough to read, a failure
    // longer, and a longer sentence longer.
    static int staysMs(Kind kind, const QString& text);
    // Not more than these at a time. The oldest goes first, but never one that says something is
    // under way: that one is the only sign of it.
    static constexpr int most = 5;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    struct Notice {
        int number = 0;
        Kind kind = Kind::Info;
        QString text;
        // Told while nobody saw the window: its time begins when somebody does.
        bool waits = false;
    };

    int add(Kind kind, const QString& text, bool waits = false);
    void remove(int number);
    // It goes by itself, after as long as one of its kind and length stays.
    void goLater(int number, Kind kind, const QString& text);

    QList<Notice> m_notices;
    int m_last = 0;
    int m_stayPercent;
    bool m_seen = true;
};

} // namespace qkvm::ui
