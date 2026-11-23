#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace qkvm::ui {

// What the box that adds a KVM to this PC's own wall is given, and what it asks for (SPEC.md
// section 4, M3a). The box only passes the wish on: whoever adds the KVM hears it, and writes here
// what came of it. Nothing here speaks to a KVM, and the password that is typed is passed on and
// kept nowhere.
//
// One for each window: what one window's box asks for and is told is nothing to another's.
//
// Made in C++ and handed to the screens; lives on the interface thread.
class AddKvmView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    // A KVM is being asked who it is and signed in to.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // Why the last one was not added, as a sentence to show; empty where it was, or none was tried.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // Refused because it says by its number that it is a KVM of the wall (core::LocalWall::saysItIsTheSame):
    // the person may keep both, as on the page (addKeepingBoth).
    Q_PROPERTY(bool mayKeepBoth READ mayKeepBoth NOTIFY changed)
    // Refused because the KVM's plain address sends everybody on to HTTPS at the same host ("https_only"): that HTTPS
    // address, which the box offers ("Use ..."), as the page does (web 0.3.2). Never taken by itself. Empty otherwise.
    Q_PROPERTY(QString useInstead READ useInstead NOTIFY changed)
    // What the last adding came to, as a sentence to show: "Build server added"; empty before one.
    Q_PROPERTY(QString added READ added NOTIFY changed)
    // What the KVM that was added last is known by from now on: its certificate's SHA-256 as the
    // page writes one, to be compared with what the KVM says of itself. Empty where it is known by
    // none (plain HTTP, or a certificate Windows trusts), and before one was added.
    Q_PROPERTY(QString certificate READ certificate NOTIFY changed)
    // How many were added since the box was opened: from the first on, its way out says "Done".
    Q_PROPERTY(int count READ count NOTIFY changed)
    // Virtual machines on Proxmox VE (wave 2): the guests the API token may see at the Proxmox typed, listed to tick
    // (listGuests), each a map of `vmid`, `node`, `name`, `status`, and `taken` where it may not be ticked (on the wall
    // as a card already, or a new virtual machine under a card's number: why, in words); and how many containers were
    // not listed. Empty before a list came.
    Q_PROPERTY(QVariantList guests READ guests NOTIFY changed)
    Q_PROPERTY(int containers READ containers NOTIFY changed)
    // A list came, with guests or with none (review c9b-1, L3): the box says what an empty one may mean.
    Q_PROPERTY(bool listed READ listed NOTIFY changed)

public:
    explicit AddKvmView(QObject* parent = nullptr);

    bool busy() const { return m_busy; }
    QString problem() const { return m_problem; }
    bool mayKeepBoth() const { return m_mayKeepBoth; }
    // (Only beside the problem it came with: a box whose problem went says nothing of it.)
    QString useInstead() const { return m_problem.isEmpty() ? QString() : m_useInstead; }
    QString added() const { return m_added; }
    QString certificate() const { return m_certificate; }
    int count() const { return m_count; }
    QVariantList guests() const { return m_guests; }
    int containers() const { return m_containers; }
    bool listed() const { return m_listed; }

    // What the box asks for. Nothing while one is being added.
    Q_INVOKABLE void add(const QString& address, const QString& user, const QString& password, const QString& group);
    // The same, to be kept beside the KVM of the wall it says it is: only where that is what it was refused for.
    Q_INVOKABLE void addKeepingBoth(const QString& address, const QString& user, const QString& password,
                                    const QString& group);
    // Of a Proxmox: its guests listed (where the address names one, that one added at once), and the ticked ones of the
    // list added, one after the other, each with what came of it said. Nothing while something is being added.
    Q_INVOKABLE void listGuests(const QString& address, const QString& tokenId, const QString& secret,
                                const QString& group);
    Q_INVOKABLE void addGuests(const QString& address, const QString& tokenId, const QString& secret,
                               const QString& group, const QVariantList& vmids);
    // The KVM that is being asked is not waited for any longer. Nothing while none is.
    Q_INVOKABLE void giveUp();
    // The box was opened: nothing of the last adding is said any more.
    Q_INVOKABLE void reset();
    // The box was closed: the KVM that is being asked is given up, and nothing of the last
    // adding is said any more. Where that KVM was added all the same (the wall had taken it in
    // that very turn), that is said otherwise, since the box says nothing now: addedAsItClosed.
    Q_INVOKABLE void closed();

    // What whoever keeps the windows says: the box is to be closed, from outside (its window is
    // out of sight). The screens close it and empty its lines; the KVM that is being asked is
    // given up and nothing of the last adding is said, whether or not there are screens.
    void close();

    // What whoever adds says of it.
    void began();
    // (`useInstead`: the HTTPS address an "https_only" offers; empty for any other refusal.)
    void refused(const QString& why, bool mayKeepBoth = false, const QString& useInstead = {});
    void took(const QString& said, const QString& certificate = {});
    // It was given up, as the box asked: nothing was added, and there is nothing to complain of.
    void gaveUp();
    // The guests a Proxmox listed (see `guests`); a run of the ticked ones begins, said with its own lines (those of
    // the run before go: review c9b-3, L3); and of them, one more added (`said`) or not (`why`), and the last of them
    // done.
    void listed(const QVariantList& guests, int containers);
    void beganARun();
    void tookGuest(const QString& said, const QString& certificate = {});
    void refusedGuest(const QString& why);
    void guestsDone();

signals:
    void changed();
    // For the screens: the box is to be closed, with nothing kept of what was typed.
    void closeAsked();
    // And: a KVM was added in the very turn in which the box was closed over it. The box says
    // nothing any more, so its window says it, as a notice: what the box would have said, and
    // the certificate it would have shown with it (empty: none).
    void addedAsItClosed(const QString& said, const QString& certificate);
    void addAsked(const QString& address, const QString& user, const QString& password, const QString& group,
                  bool keepBoth);
    void giveUpAsked();
    void listGuestsAsked(const QString& address, const QString& tokenId, const QString& secret, const QString& group);
    void addGuestsAsked(const QString& address, const QString& tokenId, const QString& secret, const QString& group,
                        const QList<qint64>& vmids);

private:
    bool m_busy = false;
    QString m_problem;
    bool m_mayKeepBoth = false;
    QString m_useInstead;
    QString m_added;
    QString m_certificate;
    int m_count = 0;
    QVariantList m_guests;
    int m_containers = 0;
    bool m_listed = false;
    // The box was closed over a KVM that was being asked, and what came of that is not known
    // yet: the box itself says nothing of it any more.
    bool m_closedOver = false;
};

} // namespace qkvm::ui
