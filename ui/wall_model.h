#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSet>
#include <QSize>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

namespace qkvm::ui {

// One machine on the wall, as its card shows it. Whoever fills the wall writes these; the card only
// shows what it is given.
struct WallTile {
    QString key;        // which machine this is, for telling a changed card from another machine's
    quint32 stream = 0; // which of the engine's streams is its picture; 0: it has none
    QString name;
    QString host;
    QString group;         // the group it was put into on the server; empty: none
    bool openable = false; // it has a console of its own that a browser can be sent to
    QString state;         // the server's word: connecting, online, no_signal, offline, auth, error, busy
    QString label;         // the same for a person to read: "Online", "No signal", "Powered off"
    QString title;         // what the card says large where there is no live picture
    QString text;          // the sentence under that
    QString action;        // the button under that; empty: none
    QString meta;          // the right end of the line under the picture
    QString largeMeta;     // the same in its large view (a PC with Intel AMT says there that it is live)
    QString amtView;       // a PC with Intel AMT: the view it is in (core::amtViewOf); empty for any other
    // How large its screen is in its own pixels, as its status says; nothing where it does not say (b10:
    // what its large view is drawn by at a scale).
    QSize pictureSize;
    QString note;       // what this PC's own wall says of it in its details (Machine::note)
    QVariantList ports; // a KVM with several computers behind it: {label, hint, shown, empty} for each
    // A server seen through its controller has no picture and shows how it is doing instead:
    // {power: "on" or "off", tone: "ok", "warn" or "bad", tag, locating, hardware,
    //  readings: [{label, value, tone}]}. Empty for a KVM.
    QVariantMap server;
    // The source it is of (Source::id: this PC, or a QKVM server); whether that source is away,
    // so that what the card shows is what was last known; and whether whoever sits here may act
    // on it. (A card nobody says that of may not be acted on, nor picked: the wall that fills the
    // cards says where it may.)
    QString source;
    bool away = false;
    bool actable = false;

    bool operator==(const WallTile&) const = default;
};

// The machines of the wall, in the wall's order, and how many of them are in each state the bar counts.
class WallModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The wall is handed its machines by the program.")
    Q_PROPERTY(int all READ all NOTIFY countsChanged)
    Q_PROPERTY(int online READ online NOTIFY countsChanged)
    Q_PROPERTY(int noSignal READ noSignal NOTIFY countsChanged)
    Q_PROPERTY(int attention READ attention NOTIFY countsChanged)
    // The groups the wall's machines are in, in the order of their names; none where no machine
    // is in one.
    Q_PROPERTY(QStringList groups READ groups NOTIFY groupsChanged)

public:
    enum Role {
        StreamRole = Qt::UserRole + 1,
        NameRole,
        HostRole,
        StateRole,
        TitleRole,
        TextRole,
        ActionRole,
        MetaRole,
        PortsRole,
        ServerRole,
        KeyRole, // which machine
        LabelRole,
        GroupRole,
        OpenableRole,
        PickedRole, // picked for something that is done to several machines at once
        SourceRole,
        AwayRole,
        ActableRole,
        NoteRole, // what this PC's own wall says of it in its details
        LargeMetaRole,
        AmtViewRole,
        PictureSizeRole,
    };

    explicit WallModel(QObject* parent = nullptr);

    // The wall as it is now. Where the machines are the same as before, in the same order, only the
    // cards that changed are told to the screen: a card is not made anew because its neighbour's
    // temperature moved.
    void setTiles(QList<WallTile> tiles);

    int all() const;
    int online() const;
    int noSignal() const;
    int attention() const;
    QStringList groups() const { return m_groups; }

    // The machines that are picked for something done to several at once, in the wall's order. A
    // machine that leaves the wall is picked no longer, nor one that may not be acted on any more.
    QStringList picked() const;
    int pickedCount() const { return static_cast<int>(m_picked.size()); }
    bool isPicked(const QString& key) const { return m_picked.contains(key); }
    // One machine, picked or not; one that is not on the wall is not picked, nor one whose card
    // says that it may not be acted on (WallTile::actable).
    void setPicked(const QString& key, bool picked);
    // These as well as those that are; and none at all.
    void pick(const QStringList& keys);
    void pickNone();

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Under which of the bar's counts a state falls, as the page has it: "online", "no_signal" or
    // "attention"; nothing for a state that is only counted under All (a machine still connecting).
    static QString countedAs(const QString& state);

signals:
    void countsChanged();
    void groupsChanged();
    void pickedChanged();

private:
    int counted(const QString& as) const;
    void lookForGroups();

    QList<WallTile> m_tiles;
    QStringList m_groups;
    QSet<QString> m_picked;
};

// The wall as the bar's choices leave it: one of the counts, and what was typed into "Find a KVM".
class WallFilter : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The wall is handed its machines by the program.")
    Q_PROPERTY(QString show READ show WRITE setShow NOTIFY showChanged)
    Q_PROPERTY(QString find READ find WRITE setFind NOTIFY findChanged)
    Q_PROPERTY(QString group READ group WRITE setGroup NOTIFY groupChanged)
    Q_PROPERTY(QString order READ order WRITE setOrder NOTIFY orderChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit WallFilter(QObject* parent = nullptr);

    // "all", or one of what WallModel::countedAs gives.
    QString show() const;
    void setShow(const QString& show);
    // A machine is found by a part of its name, of its address or of its group, whatever the case.
    QString find() const;
    void setFind(const QString& find);
    // One group's machines, or with nothing all of them. A group that no machine of the wall is in
    // narrows nothing: a choice that was remembered for another wall does not empty this one.
    QString group() const { return m_group; }
    void setGroup(const QString& group);
    // The order of the cards, as the page's View menu has it: nothing for the order the wall was
    // arranged in on the server, "name", "group" (machines without one last) or "state" (what
    // needs attention first). Anything else counts as nothing.
    QString order() const { return m_order; }
    void setOrder(const QString& order);
    // The wall's groups are not what they were: which machines a remembered group leaves may be
    // other ones now.
    void lookAgain();
    int count() const;
    // The wall's own order is being made by hand, for the source `of` (WallTile::source; nothing:
    // the cards of no source): meanwhile every machine of that source is shown, and no other,
    // whatever the bar's choices are, in the order the machines came in. (Where each card is
    // drawn meanwhile is by the order that is being made, which the wall's view holds: the cards
    // themselves stay as they are, so that one that is moved is still the card it was.)
    // Afterwards the bar's choices hold again, as they were.
    bool isArranging() const { return m_arranging; }
    void setArranging(bool arranging, const QString& of = QString());
    // The machines that are shown, first to last.
    QStringList keys() const;

signals:
    void showChanged();
    void findChanged();
    void groupChanged();
    void orderChanged();
    void countChanged();

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    QString m_show;
    QString m_find;
    QString m_group;
    QString m_order;
    bool m_arranging = false;
    QString m_arrangingOf;
};

} // namespace qkvm::ui
