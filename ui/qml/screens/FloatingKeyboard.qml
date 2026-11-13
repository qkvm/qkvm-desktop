pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Qkvm

Rectangle {
    id: keyboard

    required property ControlView control
    property real topLimit: 0
    property real bottomLimit: parent ? parent.height : 400
    property var modifiers: []
    property bool placed: false
    property Item focusedKey: null
    signal closeAsked
    // Ctrl and Alt with Delete, or Alt with F4: asked first, as the bar's buttons are
    // (Enlarged.qml, keysQuestion); `chord` is what goes on its yes, as it was pressed.
    signal keysAsked(string keys, var chord)
    objectName: "floatingKeyboard"
    color: Theme.panel
    border.color: Theme.lineStrong
    radius: 6
    z: 10
    clip: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("On-screen keyboard")

    function clamp() {
        if (!parent || !visible)
            return;
        const room = Math.max(1, bottomLimit - topLimit - 16);
        width = Math.max(Math.min(300, parent.width - 16), Math.min(width, parent.width - 16));
        height = Math.max(Math.min(180, room), Math.min(height, room));
        x = Math.max(8, Math.min(x, parent.width - width - 8));
        y = Math.max(topLimit + 8, Math.min(y, bottomLimit - height - 8));
    }
    function resetPosition() {
        width = Math.min(900, parent.width - 16);
        height = Math.min(420, bottomLimit - topLimit - 16);
        x = (parent.width - width) / 2;
        y = bottomLimit - height - 8;
        placed = true;
        clamp();
    }
    function press(usage) {
        if (!control.on || !visible)
            return;
        if (usage >= 0xe0 && usage <= 0xe3) {
            modifiers = modifiers.includes(usage) ? modifiers.filter(key => key !== usage) : modifiers.concat([usage]);
        } else {
            const chord = modifiers.concat([usage]);
            modifiers = [];
            // (Ctrl and Alt with Delete, whatever else is armed: a question too many costs little.
            // Alt with F4, and Shift or Win beside them, which still closes the window in front;
            // not with Ctrl, which is no Alt+F4: it changes the console on a Linux machine.)
            if (usage === 76 && chord.includes(224) && chord.includes(226))
                keyboard.keysAsked("ctrlAltDel", chord);
            else if (usage === 61 && chord.includes(226) && !chord.includes(224))
                keyboard.keysAsked("altF4", chord);
            else
                control.tapKeys(chord);
        }
    }
    function revealFocusedKey() {
        const key = focusedKey;
        if (!visible || !key || !key.activeFocus)
            return;
        const at = key.mapToItem(body.contentItem, 0, 0);
        let x = body.contentX;
        let y = body.contentY;
        if (at.x < x)
            x = at.x;
        else if (at.x + key.width > x + body.width)
            x = at.x + key.width - body.width;
        if (at.y < y)
            y = at.y;
        else if (at.y + key.height > y + body.height)
            y = at.y + key.height - body.height;
        body.contentX = Math.max(0, Math.min(x, body.contentWidth - body.width));
        body.contentY = Math.max(0, Math.min(y, body.contentHeight - body.height));
    }
    onVisibleChanged: {
        modifiers = [];
        if (visible) {
            if (!placed)
                resetPosition();
            else
                clamp();
        }
    }
    onTopLimitChanged: clamp()
    onBottomLimitChanged: clamp()
    Connections {
        target: keyboard.parent
        function onWidthChanged() {
            keyboard.clamp();
        }
    }
    Connections {
        target: keyboard.control
        function onInputReset() {
            keyboard.modifiers = [];
        }
    }

    // An opaque panel consumes pointer input even in the gaps between its keys.
    MouseArea {
        anchors.fill: parent
    }
    Item {
        id: titleBar
        objectName: "keyboardDrag"
        x: 1
        y: 1
        width: parent.width - 2
        height: 42
        activeFocusOnTab: true
        Accessible.name: qsTr("Move keyboard with arrow keys or drag")
        Keys.onPressed: event => {
            const step = 20;
            if (event.key === Qt.Key_Left)
                keyboard.x -= step;
            else if (event.key === Qt.Key_Right)
                keyboard.x += step;
            else if (event.key === Qt.Key_Up)
                keyboard.y -= step;
            else if (event.key === Qt.Key_Down)
                keyboard.y += step;
            else
                return;
            keyboard.clamp();
            event.accepted = true;
        }
        MouseArea {
            anchors.fill: parent
            drag.target: keyboard
            drag.minimumX: 8
            drag.maximumX: keyboard.parent.width - keyboard.width - 8
            drag.minimumY: keyboard.topLimit + 8
            drag.maximumY: keyboard.bottomLimit - keyboard.height - 8
            onPressed: titleBar.forceActiveFocus()
        }
        RowLayout {
            anchors.fill: parent
            anchors.margins: 6
            spacing: 6
            Text {
                text: qsTr("Keyboard")
                textFormat: Text.PlainText
                font.pixelSize: Theme.textSize
                font.bold: true
                color: Theme.text
                Layout.fillWidth: true
            }
            Button {
                objectName: "keyboardReset"
                text: qsTr("Reset position")
                onClicked: keyboard.resetPosition()
            }
            Button {
                objectName: "keyboardClose"
                symbol: "x"
                hint: qsTr("Close keyboard")
                onClicked: keyboard.closeAsked()
            }
        }
    }
    Flickable {
        id: body
        objectName: "keyboardBody"
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 10
        anchors.bottomMargin: 26
        contentWidth: Math.max(680, width)
        contentHeight: rows.height
        onWidthChanged: Qt.callLater(keyboard.revealFocusedKey)
        onHeightChanged: Qt.callLater(keyboard.revealFocusedKey)
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Column {
            id: rows
            width: body.contentWidth
            spacing: 4
            Text {
                text: qsTr("US key positions; text follows the remote keyboard layout. Modifiers apply to the next key.")
                textFormat: Text.PlainText
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                bottomPadding: 4
            }
            Repeater {
                model: [[["Esc", 41], ...Array.from({
                            length: 12
                        }, (_, i) => ["F" + (i + 1), 58 + i])], [["`", 53], ...Array.from({
                            length: 9
                        }, (_, i) => [String(i + 1), 30 + i]), ["0", 39], ["-", 45], ["=", 46], ["Backspace", 42]], [["Tab", 43], ..."QWERTYUIOP".split("").map(key => [key, key.charCodeAt(0) - 61]), ["[", 47], ["]", 48], ["Backslash", 49]], [["Caps Lock", 57], ..."ASDFGHJKL".split("").map(key => [key, key.charCodeAt(0) - 61]), [";", 51], ["'", 52], ["Enter", 40]], [["Shift", 225], ..."ZXCVBNM".split("").map(key => [key, key.charCodeAt(0) - 61]), [",", 54], [".", 55], ["/", 56], ["Up", 82]], [["Ctrl", 224], ["Win", 227], ["Alt", 226], ["Space", 44], ["Left", 80], ["Down", 81], ["Right", 79]], [["Insert", 73], ["Delete", 76], ["Home", 74], ["End", 77], ["Page Up", 75], ["Page Down", 78], ["PrtSc", 70]]]
                delegate: RowLayout {
                    id: keyRow
                    required property var modelData
                    width: rows.width
                    height: 40
                    spacing: 4
                    Repeater {
                        model: keyRow.modelData
                        delegate: Button {
                            id: keyButton
                            required property var modelData
                            objectName: "screenKey:" + modelData[1]
                            text: modelData[0]
                            Layout.fillWidth: true
                            Layout.preferredWidth: modelData[1] === 44 ? 220 : implicitWidth
                            Layout.minimumWidth: 40
                            Layout.fillHeight: true
                            checkable: modelData[1] >= 224 && modelData[1] <= 227
                            checked: keyboard.modifiers.includes(modelData[1])
                            primary: checked
                            enabled: keyboard.control.on
                            onActiveFocusChanged: {
                                if (activeFocus) {
                                    keyboard.focusedKey = keyButton;
                                    Qt.callLater(keyboard.revealFocusedKey);
                                } else if (keyboard.focusedKey === keyButton) {
                                    keyboard.focusedKey = null;
                                }
                            }
                            onClicked: keyboard.press(modelData[1])
                        }
                    }
                }
            }
        }
    }
    Button {
        id: resize
        objectName: "keyboardResize"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 26
        height: 26
        symbol: "maximize"
        hint: qsTr("Resize keyboard with arrow keys or drag")
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Left)
                keyboard.width -= 20;
            else if (event.key === Qt.Key_Right)
                keyboard.width += 20;
            else if (event.key === Qt.Key_Up)
                keyboard.height -= 20;
            else if (event.key === Qt.Key_Down)
                keyboard.height += 20;
            else
                return;
            keyboard.clamp();
            event.accepted = true;
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.SizeFDiagCursor
            property point origin
            property size before
            onPressed: mouse => {
                resize.forceActiveFocus();
                origin = mapToItem(keyboard.parent, mouse.x, mouse.y);
                before = Qt.size(keyboard.width, keyboard.height);
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return;
                const now = mapToItem(keyboard.parent, mouse.x, mouse.y);
                keyboard.width = before.width + now.x - origin.x;
                keyboard.height = before.height + now.y - origin.y;
                keyboard.clamp();
            }
        }
    }
}
