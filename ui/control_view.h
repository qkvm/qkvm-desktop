#pragma once

#include <cstdint>

#include <QList>
#include <QObject>
#include <QPointF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace qkvm::ui {

// Where in a machine's picture a place in its tile is. The picture keeps its shape inside the
// tile, as large as fits and in the middle (the engine draws it so: engine/src/scene_core.cpp),
// and leaves bars beside or above it. Each from 0.0 to 1.0 over the picture; less or more over a
// bar, and where the picture's size is not known.
QPointF inPicture(QPointF at, QSizeF tile, QSize picture);

// What the screen of a machine that is shown large is given for its keyboard and mouse (SPEC.md
// section 4, M3b): whether they can be taken, whether they are, and why not; and what it asks:
// that they be taken or given back, and what the pointer does over the picture while they are
// taken. It decides nothing and holds nothing but what the screen shows: what the screen asks
// goes out as a signal to whoever controls the machine (MachineControl), who writes here what
// came of it.
//
// One for each window. Made in C++ and handed to the screens; lives on the interface thread.
class ControlView : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The screens are handed this by the program.")
    // The machine that is shown large is had live, and whoever sits here may act on machines:
    // its keyboard and mouse can be taken.
    Q_PROPERTY(bool possible READ isPossible NOTIFY changed)
    // They are taken: every key of this PC's keyboard goes to the machine, and the pointer over
    // its picture. And they are asked for, and the machine has not answered yet.
    Q_PROPERTY(bool on READ isOn NOTIFY changed)
    Q_PROPERTY(bool localInput READ localInput NOTIFY changed)
    Q_PROPERTY(bool asking READ isAsking NOTIFY changed)
    // The machine still owes a word: its answer to an asking that is over, or its word for a
    // giving back. They are not asked for until it has come or its time has run out (seconds),
    // and the button that asks does nothing meanwhile.
    Q_PROPERTY(bool busy READ isBusy NOTIFY changed)
    // While they are taken: Windows' second account of the keyboard is there, so that keys which
    // Windows stopped handing over would be noticed. False where this PC gives none.
    Q_PROPERTY(bool watched READ isWatched NOTIFY changed)
    Q_PROPERTY(bool relative READ isRelative NOTIFY changed)
    // How large the machine's live pictures are as they come, in its own pixels (Shown::picture): what the
    // engine draws, and what the pointer is mapped by, fitted and at a scale. Nothing: not known.
    Q_PROPERTY(QSize pictureSize READ picture NOTIFY changed)
    Q_PROPERTY(bool mouseSelectable READ mouseSelectable NOTIFY changed)
    Q_PROPERTY(bool textAllowed READ textAllowed NOTIFY changed)
    Q_PROPERTY(bool pointerCaptured READ pointerCaptured NOTIFY changed)
    Q_PROPERTY(QString ownerName READ ownerName NOTIFY changed)
    Q_PROPERTY(bool ownerInAnotherWindow READ ownerInAnotherWindow NOTIFY changed)
    Q_PROPERTY(bool takeoverPossible READ takeoverPossible NOTIFY changed)
    Q_PROPERTY(bool takeoverPending READ takeoverPending NOTIFY changed)
    Q_PROPERTY(QString takeoverOwner READ takeoverOwner NOTIFY changed)
    // Why they were not given, or were taken away, as a sentence; empty: nothing to say.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    // What is to be known while they are taken, as a sentence; empty: nothing. (That a key which
    // was held from before keeps what is typed from the machine; that keys which Windows stops
    // handing over would not be noticed on this PC.)
    Q_PROPERTY(QString note READ note NOTIFY changed)
    // What the key is called that gives them back, whatever else happens. (Whether the keyboard
    // that is plugged in has that key cannot be known: the screen names the button for giving
    // them back beside it, always.)
    Q_PROPERTY(QString leaveKeyName READ leaveKeyName NOTIFY releaseKeyChanged)
    Q_PROPERTY(QString releaseKey READ releaseKey WRITE setReleaseKey NOTIFY releaseKeyChanged)

public:
    struct Shown {
        bool possible = false;
        bool on = false;
        bool localInput = false;
        bool asking = false;
        bool busy = false;
        bool watched = true;
        QString problem;
        QString note;
        // How large the machine's picture is, in its own pixels: where it lies in its tile goes
        // by its shape. Nothing: not known.
        QSize picture;
        bool relative = false;
        bool mouseSelectable = true;
        bool textAllowed = true;
        bool pointerCaptured = false;
        QString ownerName;
        bool ownerInAnotherWindow = false;
        bool takeoverPossible = false;
        bool takeoverPending = false;
        QString takeoverOwner;

        bool operator==(const Shown&) const = default;
    };

    explicit ControlView(QObject* parent = nullptr);

    bool isPossible() const { return m_shown.possible; }
    bool isOn() const { return m_shown.on; }
    // How large the machine's picture is, in its own pixels, as it was last shown (Shown::picture).
    QSize pictureSize() const { return m_shown.picture; }
    bool localInput() const { return m_shown.localInput; }
    bool isAsking() const { return m_shown.asking; }
    bool isBusy() const { return m_shown.busy; }
    bool isWatched() const { return m_shown.watched; }
    QString problem() const { return m_shown.problem; }
    QString note() const { return m_shown.note; }
    QSize picture() const { return m_shown.picture; }
    bool isRelative() const { return m_shown.relative; }
    bool mouseSelectable() const { return m_shown.mouseSelectable; }
    bool textAllowed() const { return m_shown.textAllowed; }
    bool pointerCaptured() const { return m_shown.pointerCaptured; }
    QString ownerName() const { return m_shown.ownerName; }
    bool ownerInAnotherWindow() const { return m_shown.ownerInAnotherWindow; }
    bool takeoverPossible() const { return m_shown.takeoverPossible; }
    bool takeoverPending() const { return m_shown.takeoverPending; }
    QString takeoverOwner() const { return m_shown.takeoverOwner; }
    QString leaveKeyName() const;
    // A machine's name as a sentence of the screen says it: set off, as the program's sentences set
    // a name off (core::exchange::nameSetOff), so that a name made to read as words cannot pass for
    // the sentence's own; and "this machine" for a name that shows nothing.
    Q_INVOKABLE static QString inASentence(const QString& name);
    QString releaseKey() const { return m_releaseKey; }
    void setReleaseKey(const QString& key);
    std::uint16_t leaveKey() const;

    // What the screen asks for. That keyboard and mouse be taken; that they be given back, or
    // no longer asked for; and that Ctrl+Alt+Del be pressed on the machine, which no program is
    // handed by Windows.
    Q_INVOKABLE void take();
    Q_INVOKABLE void answerTakeover(bool yes);
    Q_INVOKABLE void giveBack();
    Q_INVOKABLE void ctrlAltDel();
    // Local console panels keep the lease, but release remote input and stop swallowing
    // this PC's keys. Screen chords never request a lease and always release their keys.
    Q_INVOKABLE void setLocalInput(bool local);
    Q_INVOKABLE void tapKeys(const QList<int>& usages);
    // Whoever sits here went on to something else in the window: pressed a key (`byAKey`), or
    // opened a box. Where keyboard and mouse were asked for and are not given yet, they are not
    // waited for any more, and which of the two it was is what the screen is then told.
    Q_INVOKABLE void wentOn(bool byAKey = false);
    // The pointer is at this place of the tile the picture is drawn in, which is this large.
    Q_INVOKABLE void pointerAt(qreal x, qreal y, qreal width, qreal height);
    // The same where the picture is shown at a scale of its own (b10): it lies at `left`, `top`, this
    // large, in the place's own pixels, and may be larger than the place (picture_scale.h, placeIn).
    Q_INVOKABLE void pointerOver(qreal x, qreal y, qreal left, qreal top, qreal width, qreal height);
    // A button of the mouse went down or up, by Qt's name for it (Qt::MouseButton).
    Q_INVOKABLE void button(int button, bool down);
    // The wheel turned, as Qt says it: in eighths of a degree, a notch being 120 of them.
    Q_INVOKABLE void wheel(int angleX, int angleY);
    Q_INVOKABLE void setRelative(bool relative);
    void pointerBy(int dx, int dy);
    void captureReady();
    void captureLost();

    // What whoever controls the machine says.
    void show(const Shown& shown);

signals:
    void changed();
    void releaseKeyChanged();

    // For whoever controls the machine.
    void takeAsked();
    void takeoverAsked();
    void takeoverAnswered(bool yes);
    void giveBackAsked();
    void ctrlAltDelAsked();
    void localInputAsked(bool local);
    void keysTapped(const QList<int>& usages);
    void inputReset();
    void wentOnSaid(bool byAKey);
    // Where in the picture the pointer is: each 0.0 to 1.0 over it, and less or more beside it.
    void pointerMoved(double x, double y);
    // A button, 1 to 5 (left, right, middle, back, forward).
    void buttonChanged(int number, bool down);
    void wheelTurned(int angleX, int angleY);
    void relativeAsked(bool relative);
    void pointerShifted(int dx, int dy);
    void captureBegan();
    void captureEnded();

private:
    Shown m_shown;
    QString m_releaseKey = QStringLiteral("right-ctrl");
};

} // namespace qkvm::ui
