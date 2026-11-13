pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import Qkvm

// What drops from a button of the bar, as the page's menus do: rows to choose from, the chosen one
// with a tick before it, and a small heading over each kind of row. It closes when a row is chosen,
// on Esc, on a click beside it or on its button again, and when its rows are made anew. Opened from
// the keyboard, its button shows where the keys are again once it has closed.
//
// `rows` is a list of {heading}, of {divider: true}, and of {text, kind, value, chosen, hint}: a
// row that is chosen tells `picked(kind, value)`; `hint` is the key that does the same, shown at the
// row's right end. The menu belongs to the button it is declared in, and opens under it.
//
// One row is marked at a time: the one the pointer is over or the arrow keys have come to, starting
// at the chosen one. Up and Down pass over the headings and the dividing lines and go round the
// ends; Return and Space choose.
//
// It is as high as its rows, or as the window leaves under its button: then it scrolls, a mark at
// its right edge says how far, and a row the keys come to is brought into sight.
T.Popup {
    id: menu

    property var rows: []
    // Under the button's right end and growing leftwards: for a button near the window's right edge.
    property bool fromTheRight: false

    // How high the window lets it be: looked at each time it opens.
    property real room: 100000
    // It was opened from the keyboard: its button had the keys, and showed it.
    property bool byKeys: false

    signal picked(string kind, var value)

    function mark(at: int, reason: int) {
        const slot = list.itemAt(at) as Loader;
        (slot.item as Item).forceActiveFocus(reason);
        // Into sight, where the keys or the opening brought it. (The pointer marks rows without
        // this: a menu that scrolled under the pointer would not hold still.)
        if (slot.y < scroller.contentY)
            scroller.contentY = slot.y;
        else if (slot.y + slot.height > scroller.contentY + scroller.height)
            scroller.contentY = slot.y + slot.height - scroller.height;
    }

    // Opens it, or closes it where it is open: its button's click.
    function toggle() {
        if (menu.opened)
            menu.close();
        else
            menu.open();
    }

    function move(from: int, by: int) {
        for (let step = 1; step <= rows.length; ++step) {
            const at = (from + by * step + rows.length * step) % rows.length;
            if (rows[at].heading === undefined && rows[at].divider !== true) {
                mark(at, Qt.TabFocusReason);
                return;
            }
        }
    }

    x: fromTheRight ? parent.width - width : 0
    y: parent.height + 4
    width: Math.max(180, column.implicitWidth + 2 * padding)
    height: Math.min(column.implicitHeight + 2 * padding, room)
    padding: 5
    focus: true
    // (Not on a press on its button: that one's click closes it, as the page's toggles.)
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
    // Its rows are made anew while it is open (the window got wider, a group came): it closes,
    // rather than show the keys at a row that is gone.
    onRowsChanged: {
        if (menu.opened)
            menu.close();
    }

    // A choice makes the rows anew, while the menu is closed and nothing is drawn. They are put in
    // their places before the menu shows, and not with its first frame: a click that comes sooner
    // than that would find them all on one spot.
    onAboutToShow: {
        const window = T.Overlay.overlay;
        room = window ? Math.max(100, window.height - parent.mapToItem(null, 0, parent.height).y - 4 - 8) : 100000;
        column.forceLayout();
        // (Not where the pointer is over the button: a click, whatever the keys did before.)
        byKeys = ((parent as T.Control)?.visualFocus ?? false) && !((parent as T.Control)?.hovered ?? false);
    }
    // (Qt gives the keys back to the button as it closes, and as a popup's: shown again a turn
    // later, where they are still there.)
    onClosed: {
        const button = parent as T.Control;
        if (menu.byKeys && button !== null)
            Qt.callLater(() => {
                if (button.activeFocus)
                    button.focusReason = Qt.TabFocusReason;
            });
    }
    onOpened: {
        let at = rows.findIndex(row => row.chosen === true);
        if (at < 0)
            at = rows.findIndex(row => row.heading === undefined && row.divider !== true);
        if (at >= 0)
            mark(at, menu.byKeys ? Qt.TabFocusReason : Qt.PopupFocusReason);
    }

    background: Rectangle {
        radius: Theme.radius + 2
        color: Theme.panel
        border.width: 1
        border.color: Theme.lineStrong
    }

    contentItem: Flickable {
        id: scroller

        readonly property bool scrolls: contentHeight > height + 1

        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Rectangle {
            objectName: "menuScrollMark"
            parent: scroller
            visible: scroller.scrolls
            x: scroller.width - width
            y: scroller.visibleArea.yPosition * scroller.height
            width: 3
            height: Math.max(24, scroller.visibleArea.heightRatio * scroller.height)
            radius: 1.5
            color: Theme.lineStrong
        }

        Column {
            id: column

            Repeater {
                id: list

                model: menu.rows

                delegate: Loader {
                    id: slot

                    required property var modelData
                    required property int index

                    width: Math.max(implicitWidth, menu.width - 2 * menu.padding)
                    sourceComponent: modelData.heading !== undefined ? heading : modelData.divider === true ? divider : row

                    Component {
                        id: heading

                        Text {
                            textFormat: Text.PlainText
                            leftPadding: 9
                            topPadding: 8
                            bottomPadding: 4
                            text: slot.modelData.heading
                            color: Theme.muted
                            font.pixelSize: Theme.headingSize
                            font.weight: Font.DemiBold
                            font.capitalization: Font.AllUppercase
                            font.letterSpacing: 0.9
                        }
                    }
                    Component {
                        id: divider

                        Item {
                            implicitHeight: 9

                            Rectangle {
                                x: 4
                                y: 4
                                width: parent.width - 8
                                height: 1
                                color: Theme.line
                            }
                        }
                    }
                    Component {
                        id: row

                        T.Button {
                            id: choice

                            implicitWidth: tick.width + words.implicitWidth + 9 + 8 + 14 + (key.visible ? key.width + 14 : 0)
                            implicitHeight: 30
                            text: slot.modelData.text
                            Accessible.name: text
                            Accessible.checkable: true
                            Accessible.checked: slot.modelData.chosen === true
                            Keys.onDownPressed: menu.move(slot.index, 1)
                            Keys.onUpPressed: menu.move(slot.index, -1)
                            Keys.onReturnPressed: clicked()
                            Keys.onEnterPressed: clicked()
                            onHoveredChanged: {
                                if (hovered)
                                    forceActiveFocus(Qt.MouseFocusReason);
                            }
                            // Closed first: what is chosen changes the rows, and with them this one.
                            onClicked: {
                                const kind = slot.modelData.kind;
                                const value = slot.modelData.value;
                                menu.close();
                                menu.picked(kind, value);
                            }

                            contentItem: Item {
                                // (A picture of itself, a layer: a row scrolled out over the
                                // menu's edge is cut off there, and so is its tick. Check.qml says
                                // why.)
                                Icon {
                                    id: tick

                                    x: 9
                                    anchors.verticalCenter: parent.verticalCenter
                                    opacity: slot.modelData.chosen === true ? 1 : 0
                                    name: "check"
                                    size: 13
                                    color: Theme.accentText
                                    layer.enabled: true
                                }
                                Text {
                                    id: words

                                    textFormat: Text.PlainText
                                    anchors.left: tick.right
                                    anchors.leftMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: choice.text
                                    color: Theme.text
                                    font.pixelSize: Theme.textSize
                                    font.weight: slot.modelData.chosen === true ? Font.DemiBold : Font.Normal
                                }
                                // The key that does the same.
                                Rectangle {
                                    id: key

                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: (slot.modelData.hint ?? "") !== ""
                                    width: keyText.implicitWidth + 12
                                    height: 20
                                    radius: 3
                                    color: Theme.inset
                                    border.width: 1
                                    border.color: Theme.line

                                    Text {
                                        id: keyText

                                        textFormat: Text.PlainText
                                        anchors.centerIn: parent
                                        text: slot.modelData.hint ?? ""
                                        color: Theme.muted
                                        font.pixelSize: 11
                                    }
                                }
                            }
                            background: Rectangle {
                                radius: Theme.radius
                                color: choice.activeFocus || choice.hovered ? Theme.hover : "transparent"
                            }
                        }
                    }
                }
            }
        }
    }
}
