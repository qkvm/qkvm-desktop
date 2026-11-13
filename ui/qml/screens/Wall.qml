pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// The wall: the cards of the machines the bar leaves, in a grid. As on the page with its columns set
// to "Fit": as large as lets every card be seen without scrolling, in the middle of the window; and
// with too many for that, readable cards and scrolling. A wall that is only shown (display mode) is
// laid out so whatever number of columns was chosen.
Flickable {
    id: wall

    required property WallView view
    required property ActionsView actions
    // The server does not answer: what the cards show is as it was when it last did.
    property bool old: false
    // This PC's own wall, which KVMs are added to here.
    property bool ownWall: false

    // The machine that was shown large last: once none is, the keyboard is on its card, and the
    // card is in sight, as on the page (app.js, closeFocus).
    property string lastShown: ""

    // The box that adds a KVM is wished for.
    signal addAsked
    // A click on a card showed its machine large.
    signal openedByAClick

    readonly property real gap: Theme.gap
    // Room at the foot of the wall for the bar that is there while machines are being picked, or
    // the wall's order is being made.
    readonly property real foot: view.selecting || view.arranging ? 64 : 0

    // While the wall's own order is being made: the machines in that order. A card is drawn where
    // its machine stands in it, and stays the card it is when it is moved.
    readonly property var arrangement: view.arrangement

    // While a card is dragged to another place: which card, and the card and the side of it where
    // it would come; -1 where it would come nowhere.
    property string draggedKey: ""
    property string draggedName: ""
    property int dropIndex: -1
    property bool dropAfter: false
    property point dragAt

    // The keyboard is the wall's (at sign-in, as display mode begins: Main.qml), or the card's of
    // the machine shown large last ("card": back from the large view; each card hears backTo, and
    // the one it is about answers; where the card is gone, the wall has it). Where the wall is not
    // in sight (signed out meanwhile) nothing takes it, and nothing is owed. While a box lies over
    // the window, the box keeps the keyboard, and the wall or the card takes it once the box is
    // gone (owedTo).
    property bool cardHasTheKeyboard: false
    property string owedTo: ""
    signal backTo(string machine)

    function takeTheKeyboard(to: string) {
        if (!wall.visible || view.enlarged !== "") {
            wall.owedTo = "";
            return;
        }
        if (view.covered) {
            wall.owedTo = to;
            return;
        }
        wall.owedTo = "";
        wall.cardHasTheKeyboard = false;
        if (to === "card")
            wall.backTo(wall.lastShown);
        if (!wall.cardHasTheKeyboard)
            wall.forceActiveFocus();
    }
    function takeOwed() {
        if (wall.owedTo !== "")
            wall.takeTheKeyboard(wall.owedTo);
    }
    // A card, by where it stands in the grid and how high it is, whole in sight.
    function bringIntoSight(at: real, tall: real) {
        const top = grid.y + at - gap;
        const bottom = grid.y + at + tall + gap;
        if (top < contentY)
            contentY = Math.max(0, top);
        else if (bottom > contentY + height - foot)
            contentY = Math.max(0, Math.min(contentHeight - height, bottom - height + foot));
    }
    Connections {
        target: wall.view
        function onEnlargedChanged() {
            if (wall.view.enlarged !== "") {
                wall.lastShown = wall.view.enlarged;
                wall.owedTo = "";
            } else {
                // (A turn later: the wall is in sight again then.)
                Qt.callLater(wall.takeTheKeyboard, "card");
            }
        }
        // (A turn later: after the box has done with the keyboard as it goes.)
        function onCoveredChanged() {
            if (!wall.view.covered && wall.owedTo !== "")
                Qt.callLater(wall.takeOwed);
        }
    }

    // The card under a point of the window, and whether the point is in its later half.
    function cardUnder(inWindow) {
        const at = grid.mapFromItem(null, inWindow.x, inWindow.y);
        const count = view.shown.count;
        if (count === 0)
            return {
                "index": -1,
                "after": false
            };
        const column = Math.max(0, Math.min(columns - 1, Math.floor((at.x + gap / 2) / (cardWidth + gap))));
        const row = Math.max(0, Math.min(rows - 1, Math.floor((at.y + gap / 2) / (cardHeight + gap))));
        const index = Math.min(count - 1, row * columns + column);
        const left = (index % columns) * (cardWidth + gap);
        return {
            "index": index,
            "after": at.x >= left + cardWidth / 2
        };
    }
    function cardDragged(key, name, inWindow) {
        const under = cardUnder(inWindow);
        draggedKey = key;
        draggedName = name;
        dropIndex = under.index;
        dropAfter = under.after;
        dragAt = mapFromItem(null, inWindow.x, inWindow.y);
    }
    // Let go: the card comes before or after the one it was let go over, counted without itself,
    // as the page counts it.
    function cardLetGo(from) {
        if (draggedKey !== "" && dropIndex >= 0 && dropIndex !== from)
            view.placeAt(draggedKey, (dropIndex > from ? dropIndex - 1 : dropIndex) + (dropAfter ? 1 : 0));
        draggedKey = "";
        draggedName = "";
        dropIndex = -1;
    }
    readonly property var laid: view.fit(view.shown.count, width - 2 * gap, height - 2 * gap - foot, gap, view.displaying ? 0 : view.columns)
    readonly property int columns: laid.columns
    readonly property real cardWidth: laid.cardWidth
    readonly property real cardHeight: Math.round((cardWidth - 2) * 9 / 16) + Theme.barHeight + 2
    readonly property int rows: Math.ceil(view.shown.count / columns)
    readonly property real gridWidth: columns * cardWidth + (columns - 1) * gap
    readonly property real gridHeight: rows * cardHeight + (rows - 1) * gap

    // Which cards are in sight, or will be with the next turn of the wheel: a row more above and
    // below. Pictures are asked of the server for these, and for no others.
    readonly property real gridTop: Math.max(gap, (height - foot - gridHeight) / 2)
    readonly property real rowPitch: cardHeight + gap
    readonly property int firstRowInSight: Math.max(0, Math.floor((contentY - gridTop) / rowPitch) - 1)
    readonly property int lastRowInSight: Math.min(rows - 1, Math.floor((contentY + height - gridTop) / rowPitch) + 1)
    readonly property point inSight: Qt.point(firstRowInSight * columns, Math.min(view.shown.count, (lastRowInSight + 1) * columns) - 1)

    onInSightChanged: view.setInSight(inSight.x, inSight.y)
    Component.onCompleted: view.setInSight(inSight.x, inSight.y)

    // How wide a card's picture is drawn, in the monitor's own pixels: pictures are asked of the
    // server by that.
    Binding {
        target: wall.view
        property: "cardPixels"
        value: Math.max(0, Math.round((wall.cardWidth - 2) * Screen.devicePixelRatio))
    }

    contentWidth: width
    contentHeight: Math.max(height, gridHeight + 2 * gap + foot)
    boundsBehavior: Flickable.StopAtBounds
    flickableDirection: Flickable.VerticalFlick

    Item {
        id: grid

        x: Math.round((wall.width - wall.gridWidth) / 2)
        y: Math.round(wall.gridTop)
        width: wall.gridWidth
        height: wall.gridHeight

        Repeater {
            model: wall.view.shown

            delegate: Tile {
                id: tile

                required property int index
                required property string key

                // Back from the machine shown large, where it was this one.
                Connections {
                    target: wall
                    function onBackTo(machine: string) {
                        if (machine !== tile.key)
                            return;
                        wall.bringIntoSight(tile.y, tile.height);
                        tile.forceActiveFocus();
                        wall.cardHasTheKeyboard = true;
                    }
                }

                actable: wall.actions.allowed
                shortcutsAvailable: wall.view.shortcutsAvailable
                favorite: wall.view.favoriteKeys.includes(key)
                onFavoriteAsked: wall.view.toggleFavorite(key)
                selecting: wall.view.selecting
                old: wall.old
                onPickAsked: wall.view.togglePicked(key)
                onEnlargeAsked: byAClick => {
                    wall.view.enlarge(key);
                    if (byAClick && wall.view.enlarged === key)
                        wall.openedByAClick();
                }
                onActionsAsked: wall.actions.open(key)
                onActionAsked: wall.actions.cardAction(key)
                onConsoleAsked: wall.actions.openConsole(key)
                onPortAsked: port => wall.actions.showPort(key, port)

                // Where the card is drawn, counted from 0: where the bar's choices have it, or,
                // while the wall's own order is being made, where that order has its machine. (A
                // machine the order does not name yet, in the moment arranging begins: where it
                // stands among the machines as they came, which is where the order begins.)
                readonly property int placed: wall.view.arranging ? wall.arrangement.indexOf(key) : -1
                readonly property int at: placed >= 0 ? placed : index

                arranging: wall.view.arranging
                fixed: wall.view.savingOrder
                place: at + 1
                places: wall.view.shown.count
                beingDragged: wall.draggedKey === key
                dropMark: wall.draggedKey !== "" && wall.draggedKey !== key && wall.dropIndex === at ? (wall.dropAfter ? 1 : -1) : 0
                onPlaceAsked: place => wall.view.placeAt(key, place)
                onDragMoved: inWindow => wall.cardDragged(key, name, inWindow)
                onDragEnded: wall.cardLetGo(at)

                x: (at % wall.columns) * (wall.cardWidth + wall.gap)
                y: Math.floor(at / wall.columns) * (wall.cardHeight + wall.gap)
                width: wall.cardWidth

                // While the order is being made, a card that gets another place glides to it.
                Behavior on x {
                    enabled: wall.view.arranging

                    NumberAnimation {
                        duration: Theme.medium
                        easing.type: Easing.OutCubic
                    }
                }
                Behavior on y {
                    enabled: wall.view.arranging

                    NumberAnimation {
                        duration: Theme.medium
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }
    }

    // The card that is being dragged, by its name, at the pointer.
    Rectangle {
        objectName: "draggedCard"
        x: wall.dragAt.x + 14
        y: wall.dragAt.y + wall.contentY + 14
        z: 10
        width: draggedLabel.implicitWidth + 20
        height: Theme.controlHeight
        visible: wall.draggedKey !== ""
        radius: Theme.radius
        color: Theme.panel
        border.width: 1
        border.color: Theme.accentText

        Text {
            id: draggedLabel

            textFormat: Text.PlainText
            anchors.centerIn: parent
            text: wall.draggedName
            color: Theme.text
            font.pixelSize: Theme.textSize
            font.weight: Font.DemiBold
        }
    }

    // This PC's own wall with no KVM on it yet: what it is for, and the way to the first one.
    Column {
        objectName: "noKvmsYet"
        anchors.centerIn: parent
        width: Math.min(460, wall.width - 48)
        visible: wall.ownWall && wall.view.machines.all === 0
        spacing: 12

        Text {
            textFormat: Text.PlainText
            anchors.horizontalCenter: parent.horizontalCenter
            text: Words.noKvmsYet
            color: Theme.text
            font.pixelSize: 17
            font.weight: Font.DemiBold
        }
        Text {
            textFormat: Text.PlainText
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: Words.noKvmsYetHint
            color: Theme.muted
            font.pixelSize: Theme.textSize
            wrapMode: Text.WordWrap
        }
        Button {
            objectName: "addFirstKvm"
            anchors.horizontalCenter: parent.horizontalCenter
            primary: true
            text: Words.addAKvm
            onClicked: wall.addAsked()
        }
    }

    // The bar's choices leave nothing of a wall that has machines: said in its middle as on the
    // page (app.js, "no-match"), with the way back to all of them.
    Column {
        objectName: "noKvmsMatch"
        anchors.centerIn: parent
        visible: wall.view.shown.count === 0 && wall.view.machines.all > 0
        spacing: 8

        // (With the page's 6 px under it: 14 to the words, with the column's 8.)
        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 56
            height: 56 + 6

            Rectangle {
                objectName: "noMatchSign"
                width: 56
                height: 56
                radius: 6
                color: Theme.accentSoft
                border.width: 1
                border.color: Qt.rgba(Theme.accentText.r, Theme.accentText.g, Theme.accentText.b, 0.3)

                Icon {
                    anchors.centerIn: parent
                    name: "search"
                    size: 24
                    color: Theme.accentText
                }
            }
        }
        Text {
            textFormat: Text.PlainText
            anchors.horizontalCenter: parent.horizontalCenter
            text: Words.noKvmsMatch
            color: Theme.text
            font.pixelSize: 17
            font.weight: Font.DemiBold
        }
        Button {
            objectName: "showAll"
            anchors.horizontalCenter: parent.horizontalCenter
            text: Words.showAll
            // (It goes as it does what it says: the keyboard is the wall's then.)
            onClicked: {
                wall.view.shown.show = "all";
                wall.view.shown.group = "";
                wall.view.shown.find = "";
                wall.forceActiveFocus();
            }
        }
    }
    // A server's wall with nothing on it for whoever is signed in: said in its middle.
    Column {
        anchors.centerIn: parent
        visible: wall.view.machines.all === 0 && !wall.ownWall
        spacing: 8

        Text {
            textFormat: Text.PlainText
            anchors.horizontalCenter: parent.horizontalCenter
            text: Words.nothingToShow
            color: Theme.text
            font.pixelSize: 17
            font.weight: Font.DemiBold
        }
    }
}
