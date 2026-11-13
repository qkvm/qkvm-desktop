#pragma once

#include <optional>
#include <utility>

#include <QList>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "details_view.h"
#include "wall_model.h"

namespace qkvm::ui {

// What the wall's screen is given: the machines, the machines as the bar's choices leave them, the
// time of day, and how the cards are laid out. Made in C++ and handed to the screen; lives on the
// interface thread.
class WallView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The wall's screen is handed this by the program.")
    Q_PROPERTY(qkvm::ui::WallModel* machines READ machines CONSTANT)
    Q_PROPERTY(qkvm::ui::WallFilter* shown READ shown CONSTANT)
    Q_PROPERTY(QString clock READ clock NOTIFY clockChanged)
    // How wide a card's picture is on the screen, in the monitor's own pixels: the screen says it,
    // and pictures are asked of the server by it.
    Q_PROPERTY(int cardPixels READ cardPixels WRITE setCardPixels NOTIFY cardPixelsChanged)
    // How many columns the wall has: 0 for as many as let every card be seen ("Fit"), or one to
    // six, of which a narrow window gets fewer rather than cards nobody can read.
    Q_PROPERTY(int columns READ columns WRITE setColumns NOTIFY columnsChanged)
    // The machine that is shown large, in the wall's place; empty while the wall is shown. Its card,
    // as the wall's cards are given theirs: {stream, name, host, status, label, title, text, ...}.
    // And how wide its picture is drawn, in the monitor's own pixels, which the screen says.
    Q_PROPERTY(QString enlarged READ enlarged NOTIFY enlargedChanged)
    Q_PROPERTY(QStringList favoriteKeys READ favoriteKeys NOTIFY favoritesChanged)
    Q_PROPERTY(bool shortcutsAvailable READ shortcutsAvailable NOTIFY favoritesChanged)
    Q_PROPERTY(QVariantMap enlargedCard READ enlargedCard NOTIFY enlargedCardChanged)
    Q_PROPERTY(int enlargedPixels READ enlargedPixels WRITE setEnlargedPixels NOTIFY enlargedPixelsChanged)
    // How large the picture of the machine shown large is (b10): "fit", or one of the page's scales of
    // its own pixels ("50" … "200": picture_scale.h). Each machine is shown at the one last chosen for
    // it, where whoever keeps that says so; "fit" until then.
    Q_PROPERTY(QString enlargedScale READ enlargedScale NOTIFY enlargedScaleChanged)
    // The machine that is shown large is had live, and this is the stream its live pictures are
    // drawn from; 0: it is not, and its card's own stream is drawn, which its snapshots go into.
    Q_PROPERTY(quint32 liveStream READ liveStream NOTIFY liveStreamChanged)
    Q_PROPERTY(QVariantMap liveDiagnostics READ liveDiagnostics NOTIFY liveDiagnosticsChanged)
    // What the graphics card the window draws on calls itself; nothing where it has none.
    Q_PROPERTY(QString graphicsCard READ graphicsCard NOTIFY graphicsCardChanged)
    Q_PROPERTY(qkvm::ui::DetailsView* details READ details CONSTANT)
    // Its pictures come as they are made, just now. (A last one may stay up for a while after
    // they have stopped: that is drawn, and is not this.)
    Q_PROPERTY(bool live READ isLive NOTIFY liveChanged)
    // Machines are being picked for something that is done to several at once: a click on a card
    // picks it, and nothing on a card does anything else. And how many are picked.
    Q_PROPERTY(bool selecting READ selecting WRITE setSelecting NOTIFY selectingChanged)
    Q_PROPERTY(int picked READ picked NOTIFY pickedChanged)
    // How often a new picture of each machine is asked for, in milliseconds; 0: not for now
    // ("Paused"). And what there is to choose from, in milliseconds: the page's paces with the
    // server's own among them, and 0 at the end.
    Q_PROPERTY(int pace READ pace WRITE setPace NOTIFY paceChanged)
    Q_PROPERTY(QVariantList paces READ paces NOTIFY pacesChanged)
    // The screens and nothing else, for a monitor on the wall: the page's display mode. No bar,
    // and the cards as large as lets all of them be seen. Picking machines ends where it begins.
    Q_PROPERTY(bool displaying READ displaying WRITE setDisplaying NOTIFY displayingChanged)
    // While that: the groups take turns, each for twenty seconds, where the wall has more than
    // one. Chosen in display mode's own small bar.
    Q_PROPERTY(bool rotating READ rotating WRITE setRotating NOTIFY rotatingChanged)
    // Something lies over the wall, a box or a menu: the groups wait. The screen says it.
    Q_PROPERTY(bool covered READ covered WRITE setCovered NOTIFY coveredChanged)
    // Nobody can see the wall: its window is minimized, or out of sight behind the program's icon.
    // No picture is asked for meanwhile. The screen says it.
    Q_PROPERTY(bool unseen READ isUnseen WRITE setUnseen NOTIFY unseenChanged)
    // The wall's own order is being made by hand, as the page's "Arrange the tiles" has it: every
    // machine is shown, in the order as it is being made, and nothing else is done on the wall
    // meanwhile. It is saved on the server and is everybody's, so it is an admin's to do.
    Q_PROPERTY(bool arranging READ arranging NOTIFY arrangingChanged)
    // The order that was made is on its way to the server: nothing is moved, and it is not sent a
    // second time, until the server has answered.
    Q_PROPERTY(bool savingOrder READ isSavingOrder NOTIFY savingOrderChanged)
    // The machines in the order as it is being made, first to last: a card is drawn at the place
    // its machine has here. Nothing while no order is being made.
    Q_PROPERTY(QStringList arrangement READ arrangement NOTIFY arrangementChanged)

public:
    // A test lets a group's turn be over sooner.
    explicit WallView(QObject* parent = nullptr, int groupStaysMs = 20'000);

    // Which machines' cards are in sight, or about to be: the screen says it (the first and the
    // last of the cards the bar's choices leave), and pictures are asked of the server for these
    // and for no others. Until a screen has said anything, all of them are: a wall nobody has laid
    // out yet has nothing out of sight.
    Q_INVOKABLE void setInSight(int first, int last);
    QStringList inSight() const;

    QString enlarged() const { return m_enlarged; }
    QStringList favoriteKeys() const { return m_favoriteKeys; }
    bool shortcutsAvailable() const { return m_shortcutsAvailable; }
    void setFavorites(const QStringList& keys, bool available);
    Q_INVOKABLE void toggleFavorite(const QString& key) { emit favoriteAsked(key); }
    QVariantMap enlargedCard() const;
    int enlargedPixels() const { return m_enlargedPixels; }
    void setEnlargedPixels(int pixels);
    QString enlargedScale() const { return m_enlargedScale; }
    // Chosen on the screen: taken where it is one of the scales and a machine is shown large, and told
    // to whoever keeps it for that machine (scaleChosen).
    Q_INVOKABLE void chooseScale(const QString& choice);
    // What was kept for the machine that is shown large now; "fit" for anything that is no scale.
    void setEnlargedScale(const QString& choice);
    // The numbers of picture_scale.h, for the screen: a choice's scale (0 for "fit"), and where the
    // picture lies in a view of this size (its size in the machine's own pixels, `ratio` of the
    // monitor's pixels to each of the window's).
    Q_INVOKABLE static double scaleOf(const QString& choice);
    Q_INVOKABLE static QRectF pictureIn(QSizeF view, QSizeF picture, double scale, double ratio, QPointF scrolled);
    quint32 liveStream() const { return m_liveStream; }
    bool isLive() const { return m_live; }
    QVariantMap liveDiagnostics() const { return m_liveDiagnostics; }
    QString graphicsCard() const { return m_graphicsCard; }
    void setGraphicsCard(const QString& card);
    DetailsView* details() { return &m_details; }
    void setLiveDiagnostics(const QVariantMap& diagnostics);
    void setLive(bool live);
    void setLiveStream(quint32 stream);
    // Shows this machine large; one that is not on the wall is not shown.
    Q_INVOKABLE void enlarge(const QString& machine);
    // Back to the wall.
    Q_INVOKABLE void closeEnlarged();
    // The machine before or after it, in the order of the cards the bar's choices leave, and round
    // again at either end.
    Q_INVOKABLE void stepEnlarged(int by);

    bool selecting() const { return m_selecting; }
    // Leaving it lets go of every machine that was picked.
    void setSelecting(bool selecting);
    int picked() const { return m_machines.pickedCount(); }
    // The machines that are picked, in the wall's order.
    QStringList pickedMachines() const { return m_machines.picked(); }
    // Picks this machine, or lets go of it; nothing while machines are not being picked.
    Q_INVOKABLE void togglePicked(const QString& machine);
    // All that the bar's choices leave on the wall, as well as those that are picked already; and
    // none at all.
    Q_INVOKABLE void pickShown();
    Q_INVOKABLE void pickNone();

    bool arranging() const { return m_shown.isArranging(); }
    bool isSavingOrder() const { return m_savingOrder; }
    // (A machine that came to the wall since it was begun is last; one that left is not in it.
    // Only machines of the source it is made for are in it: arrangingOf.)
    QStringList arrangement() const;
    // An order is made for one source of the wall's at a time (WallTile::source): the one whose
    // order is made where one is begun from now on, which whoever has the sources says. Nothing:
    // of the cards that are of no source (a wall of one place, as tests make it).
    void setOrderOf(const QString& source) { m_orderOf = source; }
    QString orderOf() const { return m_orderOf; }
    // The source the order that is being made is for: the one it was begun for, whatever is
    // said afterwards (setOrderOf); nothing while none is being made.
    QString arrangingOf() const { return arranging() ? m_arrangingOf : QString(); }
    // Begins it, for the source an order is made of (orderOf), with every machine of that source
    // in the wall's own order, and no other machine. Not on a wall without machines of it.
    // Whatever else the wall was doing is over: no machine is shown large, none is picked, and the
    // wall is not only shown.
    Q_INVOKABLE void arrange();
    // Puts a machine at a place among all of them, counted from 0: the others close up behind it
    // and make room for it. A place before the first is the first, one after the last the last.
    Q_INVOKABLE void placeAt(const QString& machine, int place);
    // Leaves it without saving: the wall is as it was before.
    Q_INVOKABLE void stopArranging();
    // "Save this order": whoever speaks to the server is asked to send it, and says how that went.
    Q_INVOKABLE void saveOrder();
    // The server took it: the wall is left in its own order, which is the one just made, whatever
    // order the bar had chosen before. And it did not: the order stays as made, to be sent again
    // or given up.
    void orderSaved();
    void orderRefused();

    bool displaying() const { return m_displaying; }
    void setDisplaying(bool displaying);
    bool rotating() const { return m_rotating; }
    void setRotating(bool rotating);
    bool covered() const { return m_covered; }
    void setCovered(bool covered);
    bool isUnseen() const { return m_unseen; }
    void setUnseen(bool unseen);
    // A group is being shown because its turn came, and not because somebody chose it: whoever
    // remembers which group was chosen does not take it for that.
    bool isTurningNow() const { return m_turning; }

    int pace() const { return ownPace().value_or(m_serverPace); }
    // One of the choices, or 0 for a pause; anything else is not taken. A pause is for now: the
    // pace that was chosen before it stays the chosen one.
    void setPace(int everyMs);
    QVariantList paces() const;
    // The pace the server names: what holds until another is chosen here, and one of the choices.
    void setServerPace(int everyMs);
    // The paces the other sources of the wall name (where it has the machines of several): each
    // one of the choices as well.
    void setOtherPaces(const QList<int>& everyMs);
    // What whoever asks for the pictures is told: nothing while the server's pace holds, 0 while
    // paused, and otherwise the pace that was chosen.
    std::optional<int> ownPace() const;
    // The pace that was chosen here, for remembering it: 0 while none was. A pause is not one. One
    // that is remembered and is not among the choices is not used, and is kept.
    int chosenPace() const { return m_chosenPace; }
    void setChosenPace(int everyMs);
    // The pace another of the program's walls has, the pace chosen there and its pause: how often
    // pictures are asked for is one choice for all of them (WallPace).
    void paceAs(const WallView& other);

    WallModel* machines() { return &m_machines; }
    WallFilter* shown() { return &m_shown; }
    // The time of day as this PC writes it.
    QString clock() const;
    int cardPixels() const { return m_cardPixels; }
    void setCardPixels(int pixels);
    int columns() const { return m_columns; }
    void setColumns(int columns);

    // How `count` cards are laid out in a wall that is `width` by `height` inside, with `gap`
    // between them: {columns, cardWidth}. As the page does it. With `wanted` 0, its "Fit": the
    // largest cards with which every one is seen without scrolling; and where that would make them
    // too small to read, readable cards and scrolling instead. With a number, that many columns,
    // or as many of them as the window is wide enough for, and scrolling.
    Q_INVOKABLE static QVariantMap fit(int count, qreal width, qreal height, qreal gap, int wanted = 0);

    // What a card needs under its picture: the line with the name, and its own frame.
    static constexpr int underThePicture = 36 + 2;

signals:
    void favoritesChanged();
    void favoriteAsked(const QString& key);
    void revealAsked();
    void clockChanged();
    void cardPixelsChanged();
    void columnsChanged();
    void inSightChanged();
    void enlargedChanged();
    void enlargedCardChanged();
    void enlargedPixelsChanged();
    void enlargedScaleChanged();
    // Another size was chosen for the machine shown large (by its key), for whoever keeps it.
    void scaleChosen(const QString& machine, const QString& choice);
    void liveStreamChanged();
    void liveDiagnosticsChanged();
    void graphicsCardChanged();
    void liveChanged();
    void selectingChanged();
    void pickedChanged();
    // The pace, or what whoever asks for the pictures is told, is not what it was.
    void paceChanged();
    void pacesChanged();
    void chosenPaceChanged();
    void displayingChanged();
    void rotatingChanged();
    void coveredChanged();
    void unseenChanged();
    void arrangingChanged();
    void savingOrderChanged();
    void arrangementChanged();
    // This order is to be the wall's own: the machines, first to last.
    void orderAsked(const QStringList& machines);

private:
    // The row of a machine among all the wall's machines; -1 when it is not there.
    int rowOf(const QString& machine) const;
    void lookAtTheEnlargedOne();
    // The next group's turn has come.
    void turnToTheNextGroup();
    // The clock is looked at again a moment after the next second has begun.
    void waitForTheNextSecond();
    // The paces there are to choose from, without the pause.
    QList<int> offeredPaces() const;
    // Does it, and says so where the pace is not what it was.
    template <typename Change>
    void changePace(Change change);

    WallModel m_machines;
    WallFilter m_shown;
    QTimer m_everySecond;
    int m_cardPixels = 0;
    int m_columns = 0;
    std::optional<std::pair<int, int>> m_inSight;
    QString m_enlarged;
    QStringList m_favoriteKeys;
    bool m_shortcutsAvailable = false;
    int m_enlargedPixels = 0;
    QString m_enlargedScale = QStringLiteral("fit");
    quint32 m_liveStream = 0;
    QVariantMap m_liveDiagnostics;
    QString m_graphicsCard;
    DetailsView m_details{this};
    bool m_live = false;
    bool m_selecting = false;
    int m_serverPace = 1000;
    QList<int> m_otherPaces;
    int m_chosenPace = 0;
    bool m_paused = false;
    bool m_displaying = false;
    bool m_rotating = false;
    bool m_covered = false;
    bool m_unseen = false;
    bool m_savingOrder = false;
    // The order as it was made so far, by the machines that were on the wall then.
    QStringList m_made;
    // Of which source an order is made where one is begun; and the one that is being made is for.
    QString m_orderOf;
    QString m_arrangingOf;
    bool m_turning = false;
    QTimer m_turn;
};

} // namespace qkvm::ui
