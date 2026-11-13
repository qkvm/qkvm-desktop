import QtQuick
import QtQuick.Templates as T
import Qkvm

// One machine's card on the wall, as the page has it: its screen, and under it a line with the
// state's dot, the name, the address and what else is known. A click, or Enter or Space with the
// keyboard on the card, shows the machine large, as on the page; a click on one of the card's own
// buttons is that button's alone. Where whoever is signed in may act on machines, the way into
// the machine's menu is in the picture's corner while the pointer or the keyboard is on the card. While machines are being picked for something done to several of them, a click on
// the card picks it or lets go of it, and nothing on the card does anything else. And while the
// wall's own order is being made, the card says its place and is moved to another: dragged there,
// stepped one place, or by its place typed in.
Item {
    id: card

    required property int stream
    required property string name
    required property string host
    required property string status
    required property string title
    required property string text
    required property string action
    required property string meta
    required property var ports
    required property var server
    required property bool openable
    required property bool picked
    // Whoever is signed in may act on machines.
    property bool actable: false
    property bool shortcutsAvailable: false
    property bool favorite: false
    // Machines are being picked.
    property bool selecting: false
    // What the card shows is as it was a while ago: the server does not answer.
    property bool old: false
    // The wall's own order is being made; and nothing of it is changed just now, while it is on
    // its way to the server.
    property bool arranging: false
    property bool fixed: false
    // The card's place among all of them, counted from 1, and how many there are.
    property int place: 0
    property int places: 0
    // This card is the one that is being dragged; and where another is dragged over this one, the
    // side of it the other would come to: -1 before it, 1 after it.
    property bool beingDragged: false
    property int dropMark: 0

    readonly property color topLine: status === "no_signal" || status === "busy" ? Theme.warn : status === "offline" || status === "auth" || status === "error" ? Theme.bad : "transparent"

    signal actionAsked
    // The machine is to be shown large: by a click on the card, or by a key.
    signal enlargeAsked(bool byAClick)
    signal actionsAsked
    signal consoleAsked
    signal portAsked(int port)
    signal pickAsked
    signal favoriteAsked
    // Another place for this card, counted from 0.
    signal placeAsked(int place)
    // The card is being dragged, and the pointer is here in the window; and it was let go.
    signal dragMoved(point inWindow)
    signal dragEnded

    implicitHeight: Math.round((width - 2) * 9 / 16) + Theme.barHeight + 2
    opacity: beingDragged ? 0.35 : 1
    // (While the order is being made, the keyboard goes to the card's place and its two buttons.)
    activeFocusOnTab: !arranging
    Accessible.role: selecting ? Accessible.CheckBox : Accessible.Button
    Accessible.name: selecting ? Words.selectNamed.arg(name) : Words.pressEnterToEnlarge.arg(name)
    Accessible.checkable: selecting
    Accessible.checked: picked
    Keys.onReturnPressed: selecting ? card.pickAsked() : card.enlargeAsked(false)
    Keys.onEnterPressed: selecting ? card.pickAsked() : card.enlargeAsked(false)
    Keys.enabled: !arranging
    Keys.onSpacePressed: selecting ? card.pickAsked() : card.enlargeAsked(false)

    HoverHandler {
        id: pointer
    }
    // A click on the card shows the machine large, as on the page (app.js: a click on a tile that
    // is not on one of its buttons). Under everything else on the card: its buttons, its
    // computers' numbers and the button of what it says take their own clicks first, and the
    // click is theirs alone; and so does what lies over the wall: its bars, a notice. While
    // machines are picked, or the order is made, what lies over the whole card takes the click
    // instead.
    MouseArea {
        objectName: "cardOpens"
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            card.forceActiveFocus();
            card.enlargeAsked(true);
        }
    }

    CardScreen {
        id: screen

        x: 1
        y: 1
        width: card.width - 2
        height: Math.round(width * 9 / 16)
        stream: card.stream
        status: card.status
        title: card.title
        text: card.text
        action: card.action
        ports: card.ports
        server: card.server
        old: card.old
        onActionAsked: card.actionAsked()
        onPortAsked: port => card.portAsked(port)
    }
    T.Button {
        id: favoriteButton

        objectName: "cardFavorite"
        x: 9
        y: 9
        width: 28
        height: 28
        visible: card.shortcutsAvailable && !card.selecting && !card.arranging && (card.favorite || pointer.hovered || card.activeFocus || activeFocus)
        Accessible.name: card.favorite ? qsTr("Remove %1 from favorites").arg(card.name) : qsTr("Add %1 to favorites").arg(card.name)
        Accessible.checkable: true
        Accessible.checked: card.favorite
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        onClicked: card.favoriteAsked()

        contentItem: Item {
            Icon {
                anchors.centerIn: parent
                name: "star"
                size: 16
                color: card.favorite ? Theme.accentText : "#cbd5e1"
            }
        }
        background: Rectangle {
            radius: Theme.radius
            color: favoriteButton.hovered || favoriteButton.down ? "#334155" : Theme.screenPanel
            border.width: favoriteButton.visualFocus ? 2 : 1
            border.color: favoriteButton.visualFocus ? Theme.accentText : Theme.screenLine
        }
    }
    // The way into the machine's own console, in a browser: for everybody, where it has one.
    T.Button {
        id: door

        objectName: "cardConsole"
        x: card.width - width - 9 - (tool.visible ? tool.width + 6 : 0)
        y: 9
        width: 28
        height: 28
        visible: card.openable && !card.selecting && !card.arranging && (pointer.hovered || card.activeFocus || activeFocus || tool.activeFocus)
        Accessible.name: Words.openTheConsoleOf.arg(card.name)
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        onClicked: card.consoleAsked()

        contentItem: Item {
            Icon {
                anchors.centerIn: parent
                size: 15
                name: "external"
                color: door.hovered || door.visualFocus ? "#ffffff" : "#cbd5e1"
            }
        }
        background: Rectangle {
            radius: Theme.radius
            color: door.hovered || door.down ? "#334155" : Theme.screenPanel
            border.width: door.visualFocus ? 2 : 1
            border.color: door.visualFocus ? Theme.accentText : Theme.screenLine
        }
    }
    // The way into the machine's menu.
    T.Button {
        id: tool

        objectName: "cardActions"
        x: card.width - width - 9
        y: 9
        width: 28
        height: 28
        visible: card.actable && !card.selecting && !card.arranging && (pointer.hovered || card.activeFocus || activeFocus || door.activeFocus)
        Accessible.name: Words.actionsFor.arg(card.name)
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        onClicked: card.actionsAsked()

        contentItem: Item {
            Icon {
                anchors.centerIn: parent
                size: 15
                name: "zap"
                color: tool.hovered || tool.visualFocus ? "#ffffff" : "#cbd5e1"
            }
        }
        background: Rectangle {
            radius: Theme.radius
            color: tool.hovered || tool.down ? "#334155" : Theme.screenPanel
            border.width: tool.visualFocus ? 2 : 1
            border.color: tool.visualFocus ? Theme.accentText : Theme.screenLine
        }
    }

    // The line under the picture.
    Rectangle {
        id: footer

        // As on the page: what else is known keeps its place at the right, and the name and the
        // address share what is left of the line, each giving way by its own share.
        readonly property real room: metaText.x - nameText.x - 8 - 14
        readonly property real squeeze: Math.max(0, Math.min(1, room / Math.max(1, nameText.implicitWidth + hostText.implicitWidth)))

        x: 1
        y: screen.y + screen.height
        width: card.width - 2
        height: Theme.barHeight
        color: Theme.panel

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.line
        }
        Rectangle {
            id: dot

            x: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: 8
            radius: 4
            color: Theme.stateColor(card.status)
        }
        Text {
            id: metaText

            textFormat: Text.PlainText
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: card.meta
            color: Theme.muted
            font.pixelSize: Theme.smallSize
        }
        Text {
            id: nameText

            textFormat: Text.PlainText
            anchors.left: dot.right
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: implicitWidth * footer.squeeze
            text: card.name
            color: Theme.text
            font.pixelSize: Theme.textSize
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            id: hostText

            objectName: "cardHost"
            textFormat: Text.PlainText
            anchors.left: nameText.right
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: implicitWidth * footer.squeeze
            // (Not a second time where the machine is called by where it is.)
            text: card.host === card.name ? "" : card.host
            color: Theme.muted
            font.pixelSize: Theme.smallSize
            elide: Text.ElideRight
        }
    }

    // The card's frame, over everything: the picture reaches to its inner edge.
    Rectangle {
        anchors.fill: parent
        radius: Theme.radius
        color: "transparent"
        border.width: pointer.hovered || card.activeFocus || card.picked ? 2 : 1
        border.color: pointer.hovered || card.activeFocus || card.picked ? Theme.accentText : Theme.lineStrong
    }
    // A line along the top edge says at a glance when a card is not simply live.
    Rectangle {
        x: 1
        y: 1
        width: card.width - 2
        height: 2
        color: card.topLine
    }

    // While machines are being picked: whether this one is, in the corner the page has it in; and
    // over the whole card, what takes the click, so that nothing under it does.
    Rectangle {
        objectName: "cardTick"
        x: 9
        y: 9
        width: 24
        height: 24
        visible: card.selecting
        radius: Theme.radius
        color: card.picked ? "#1d4ed8" : Theme.screenPanel
        border.width: 1
        border.color: card.picked ? "#2563eb" : Theme.screenLine

        Icon {
            anchors.centerIn: parent
            visible: card.picked
            name: "check"
            size: 14
            color: "#ffffff"
        }
    }
    MouseArea {
        anchors.fill: parent
        enabled: card.selecting
        onClicked: {
            card.forceActiveFocus();
            card.pickAsked();
        }
    }

    // While the wall's own order is being made: over the whole card, its place among all of them
    // to type another into, a step to either side, and its name. The layer is what is taken hold
    // of to drag the card, and nothing under it takes a click.
    Rectangle {
        id: arrangeLayer

        objectName: "cardArrange"
        anchors.fill: parent
        visible: card.arranging
        radius: Theme.radius
        color: "#e0020617"

        // Taken hold of anywhere but on its buttons and its number, the card is dragged: once the
        // pointer has left the spot it was pressed at, where it is is said until it is let go.
        // (The wall under it would otherwise take a drag for a push to scroll.)
        MouseArea {
            id: mover

            property bool active: false
            property point pressedAt

            function letGo() {
                if (active) {
                    active = false;
                    card.dragEnded();
                }
            }

            anchors.fill: parent
            preventStealing: true
            cursorShape: card.fixed ? Qt.ArrowCursor : active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            onPressed: mouse => pressedAt = Qt.point(mouse.x, mouse.y)
            onPositionChanged: mouse => {
                if (card.fixed)
                    return;
                if (!active && Math.abs(mouse.x - pressedAt.x) + Math.abs(mouse.y - pressedAt.y) < Application.styleHints.startDragDistance)
                    return;
                active = true;
                card.dragMoved(mapToItem(null, mouse.x, mouse.y));
            }
            onReleased: letGo()
            onCanceled: letGo()
        }
        Column {
            anchors.centerIn: parent
            spacing: 10

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8

                Button {
                    objectName: "placeEarlier"
                    anchors.verticalCenter: parent.verticalCenter
                    symbol: "chevronLeft"
                    hint: Words.onePlaceEarlier.arg(card.name)
                    enabled: !card.fixed && card.place > 1
                    onClicked: card.placeAsked(card.place - 2)
                }
                T.TextField {
                    id: placeField

                    objectName: "placeField"
                    anchors.verticalCenter: parent.verticalCenter
                    width: 72
                    height: 46
                    horizontalAlignment: TextInput.AlignHCenter
                    verticalAlignment: TextInput.AlignVCenter
                    color: "#f8fafc"
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.textOnAccent
                    font.family: Application.font.family
                    font.pixelSize: 24
                    font.weight: Font.Bold
                    selectByMouse: true
                    inputMethodHints: Qt.ImhDigitsOnly
                    readOnly: card.fixed
                    // The place the card has, until another is typed; and again once it has moved,
                    // or what was typed is given up.
                    text: String(card.place)
                    Accessible.name: Words.placeOf.arg(card.name)
                    onActiveFocusChanged: {
                        if (activeFocus)
                            selectAll();
                        else
                            text = Qt.binding(() => String(card.place));
                    }
                    // A number that is no place is the nearest one: before the first is the first.
                    onAccepted: {
                        const typed = parseInt(text);
                        text = Qt.binding(() => String(card.place));
                        if (!isNaN(typed))
                            card.placeAsked(typed - 1);
                    }

                    validator: IntValidator {
                        bottom: 0
                        top: 99999
                    }
                    background: Rectangle {
                        radius: Theme.radius
                        color: "#0f172a"
                        border.width: placeField.activeFocus ? 2 : 1
                        border.color: placeField.activeFocus ? Theme.accentText : "#475569"
                    }
                }
                Button {
                    objectName: "placeLater"
                    anchors.verticalCenter: parent.verticalCenter
                    symbol: "chevronRight"
                    hint: Words.onePlaceLater.arg(card.name)
                    enabled: !card.fixed && card.place < card.places
                    onClicked: card.placeAsked(card.place)
                }
            }
            Text {
                textFormat: Text.PlainText
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(implicitWidth, arrangeLayer.width * 0.9)
                text: card.name
                color: "#f8fafc"
                font.pixelSize: Theme.textSize
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }
        // Where a card that is dragged over this one would come: a line at that side.
        Rectangle {
            objectName: "dropLine"
            x: card.dropMark > 0 ? parent.width - width : 0
            width: 5
            height: parent.height
            visible: card.dropMark !== 0
            radius: 2
            color: Theme.accentText
        }
    }
}
