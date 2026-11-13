pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import Qkvm

// One of many, as a list that drops from a button: for a choice with more entries than a row of
// buttons holds. `choices` is a list of {value, label}; `value` is the chosen one's. The list is as
// wide as the longest of them, stays inside the window, and scrolls; Up and Down move through it,
// Return and Space choose, Esc closes it without choosing; a click on the choice again closes it too.
// Opened from the keyboard, the choice shows where the keys are again once its list has closed.
// With a `symbol`, its icon stands before the chosen one, as before the page's picks in its View
// (app.js, "#order-pick" and the two after it). `arrowsStep`: Up and Down are those of what it
// stands in (a Panel goes from part to part with them), and Space and Return open its list.
T.Button {
    id: control

    property var choices: []
    property var value
    property string hint
    property string symbol
    property bool arrowsStep: false

    readonly property int at: choices.findIndex(choice => choice.value === control.value)
    // Its list is open.
    readonly property bool opened: list.opened
    // How wide the longest of the labels is, as the chosen one is written.
    readonly property real widest: choices.reduce((most, choice) => Math.max(most, letters.advanceWidth(choice.label)), 0)

    signal picked(var value)

    implicitWidth: Math.max(160, words.implicitWidth + 44 + (symbol !== "" ? 18 : 0))
    implicitHeight: Theme.controlHeight
    font.family: Application.font.family
    font.pixelSize: Theme.textSize
    font.weight: Font.Medium
    text: at >= 0 ? choices[at].label : ""
    Accessible.name: hint !== "" ? hint : text
    Accessible.description: hint !== "" ? text : ""
    Keys.onDownPressed: event => {
        if (control.arrowsStep)
            event.accepted = false;
        else
            list.open();
    }
    Keys.onReturnPressed: list.open()
    Keys.onEnterPressed: list.open()
    onClicked: list.visible ? list.close() : list.open()

    FontMetrics {
        id: letters

        font.family: control.font.family
        font.pixelSize: Theme.textSize
        font.weight: Font.DemiBold
    }

    contentItem: Item {
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            x: 9
            visible: control.symbol !== ""
            name: control.symbol
            color: Theme.muted
            // (Each icon of a choice is a picture of itself, a layer, as Check.qml says: a choice
            // may stand in what scrolls, as in the bar's View.)
            layer.enabled: true
        }
        Text {
            id: words

            textFormat: Text.PlainText
            x: control.symbol !== "" ? 30 : 12
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, parent.width - x - 28)
            text: control.text
            font: control.font
            color: Theme.text
            elide: Text.ElideRight
        }
        Icon {
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            name: "chevronDown"
            size: 12
            color: Theme.muted
            layer.enabled: true
        }
    }
    background: Rectangle {
        radius: Theme.radius
        color: control.hovered || control.down || list.opened ? Theme.hover : Theme.panel
        border.width: 1
        border.color: Theme.lineStrong

        // Where the keyboard is: the page's outline, two pixels out.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: Theme.radius + 4
            color: "transparent"
            border.width: 2
            border.color: Theme.accentText
            visible: control.visualFocus
        }
    }

    T.Popup {
        id: list

        y: control.height + 4
        // (A row is its tick, its label, and room at either side.)
        width: Math.max(control.width, 180, Math.ceil(control.widest) + 44 + 2 * padding)
        height: Math.min(rows.contentHeight + 2 * padding, 286)
        padding: 5
        // Under its button where the window has the room, and inside the window in any case.
        margins: 6
        // (Opened from the keyboard.)
        property bool byKeys: false

        focus: true
        // (Not on a press on the choice: its click closes the list.)
        closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
        // It starts at the chosen one, and with that one in sight.
        onAboutToShow: {
            // (Not where the pointer is over the choice: a click, whatever the keys did before.)
            byKeys = control.visualFocus && !control.hovered;
            rows.currentIndex = Math.max(0, control.at);
            rows.positionViewAtIndex(rows.currentIndex, ListView.Center);
        }
        onOpened: rows.forceActiveFocus()
        // (Qt gives the keys back to the choice as the list closes, and as a popup's: shown again
        // a turn later, where they are still there.)
        onClosed: {
            if (byKeys)
                Qt.callLater(() => {
                    if (control.activeFocus)
                        control.focusReason = Qt.TabFocusReason;
                });
        }

        background: Rectangle {
            radius: Theme.radius + 2
            color: Theme.panel
            border.width: 1
            border.color: Theme.lineStrong
        }

        contentItem: ListView {
            id: rows

            objectName: "choices"
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: control.choices
            keyNavigationWraps: true
            highlightMoveDuration: 0
            Keys.onReturnPressed: list.choose(currentIndex)
            Keys.onEnterPressed: list.choose(currentIndex)
            Keys.onSpacePressed: list.choose(currentIndex)

            delegate: Rectangle {
                id: row

                required property var modelData
                required property int index

                objectName: "choice"
                width: rows.width
                height: 30
                radius: Theme.radius
                color: rows.currentIndex === index ? Theme.hover : "transparent"
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.label

                Icon {
                    id: tick

                    x: 9
                    anchors.verticalCenter: parent.verticalCenter
                    opacity: row.index === control.at ? 1 : 0
                    name: "check"
                    size: 13
                    color: Theme.accentText
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.left: tick.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.label
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    font.weight: row.index === control.at ? Font.DemiBold : Font.Normal
                }
                // It takes the click for itself. A handler that only listens leaves the click to
                // whatever lies under the list as well: a card of the wall took the keyboard out
                // of the box the list belongs to.
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: rows.currentIndex = row.index
                    onClicked: list.choose(row.index)
                }
            }
        }

        function choose(index: int) {
            const chosen = control.choices[index];
            list.close();
            if (chosen !== undefined)
                control.picked(chosen.value);
        }
    }
}
