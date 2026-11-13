#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <qkvm/core/local_wall.h>

namespace qkvm::ui {

// The KVMs of this PC's own wall as the settings box lists them (its section "KVMs"): for each, what
// it is called, where it is, how it is spoken to, and what is wrong with it that a list says: that two
// of the wall are one device (each is known by the certificate the other is known by), and that no
// password is kept for it. Nothing of it is secret.
//
// The list reads no password. Whether one is kept is the wall's mark beside each KVM in the settings,
// which is no secret (core::LocalWall::marksAPasswordFor): of every KVM, whether it was asked what it
// is yet or not (one taken from a file and off is the one most likely to have none). And where the
// wall's driver of a KVM said at its last look that it has none ("no_password",
// core::LocalWall::foundWithoutAPassword), that wins over the mark: Windows may have lost what it
// kept. Both are read of the wall as it is, without asking a KVM anything, whenever a row is made or
// follows its KVM, and when the wall says that its KVMs came to something (a first look has ended).
// So each row has the mark from the start; what a look found may come a moment later, and no row is
// higher for it (Listed.qml).
//
// The list holds still while it is shown: a KVM that is changed is changed in its row, one that is
// added comes last, and one that is taken off the wall keeps its row, which says so, with its buttons
// there and taking no click. The wall's order, and what was taken off, are taken anew only when the
// list is shown anew. (Rows that moved up into the place of one that was taken off, or that changed
// places with the wall's order, would bring another KVM's "Edit" or "Remove" under a pointer that was
// on its way to the one that stood there.) And where the wall is then what the rows are, KVM for KVM
// in their order, the rows stay the ones they are, each as its KVM is now: none is made anew, and the
// keyboard stays at a button of one.
//
// One for the program, which every window's box is handed: what the list shows it shows alike in
// every window, and it holds still for as long as any box shows it. On the interface thread. The wall
// outlives it.
class OwnKvmsView : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    // How many rows there are, those of KVMs that were taken off while it was shown among them.
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1, // the wall's name for it
        NameRole,                   // what it is called: its name, or where it is where it has none
        HostRole,                   // where it is, as its card says it
        WayRole,                    // how it is spoken to: "certificate", "windows" (Windows' word) or "plain"
        TwinsRole,                  // the others of the wall that are the same device, as a sentence calls each,
                                    // with ", " between them; empty: none
        NoPasswordRole,             // no password is kept for it: the wall's mark, or what its driver found
        GoneRole,                   // it was taken off the wall while the list was shown
        FamilyRole,                 // its family (core::OwnKvm::kind): "kvmd" or "jetkvm"
    };

    explicit OwnKvmsView(QObject* parent = nullptr);

    // The wall whose KVMs these are, from now on; none: the list is empty.
    void use(core::LocalWall* wall);

    int count() const { return static_cast<int>(m_rows.size()); }
    // Whether any box shows the list just now.
    bool isShown() const { return m_shownBy > 0; }

    // A box shows the list, and no longer: each box that begins ends once. Shown anew after no box
    // showed it, the list is the wall's as it is now.
    Q_INVOKABLE void begin();
    Q_INVOKABLE void end();

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    struct Row {
        QString id;
        QString name;
        QString host;
        QString way;
        QString twins;
        QString family;
        bool noPassword = false;
        bool gone = false;

        bool operator==(const Row&) const = default;
    };

    // The KVMs of the wall that are known by a certificate, by that certificate, each in the wall's
    // order: those among whom one finds its twins. (Gone through once for all the rows, and not for
    // each: a wall holds two thousand.)
    using Alike = QHash<QByteArray, QList<core::OwnKvm>>;
    static Alike alikeIn(const QList<core::OwnKvm>& all);
    // A KVM of the wall as its row says it, among the others that are known by its certificate (for
    // whom it is the same device as), with what the wall says of its password: its mark, and the
    // KVMs whose driver found none (`foundWithout`, of the wall as it is, once for all the rows).
    Row rowOf(const core::OwnKvm& kvm, const Alike& alike, const QSet<QString>& foundWithout) const;
    // The rows anew, of the wall as it is, in its order.
    void takeAnew();
    // The wall changed while the list is shown, or said that its KVMs came to something: each row
    // follows its KVM in its place.
    void follow();

    QPointer<core::LocalWall> m_wall;
    QMetaObject::Connection m_heard;
    QMetaObject::Connection m_cameToSomething;
    QList<Row> m_rows;
    int m_shownBy = 0;
};

} // namespace qkvm::ui
