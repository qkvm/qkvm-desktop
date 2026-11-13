import QtQuick
import QtQuick.Templates as T
import Qkvm

// One thing that can be done to a machine, as the page draws them in a machine's menu: its icon in a
// small tinted box, what it is called, and at the right the keys it sends or how it stands. Red
// where it can lose somebody's work. Space and Return choose it.
T.Button {
    id: control

    property string symbol
    property string hint
    property bool danger: false

    implicitWidth: 230
    implicitHeight: 42
    font.family: Application.font.family
    font.pixelSize: Theme.textSize
    font.weight: Font.Medium
    opacity: enabled ? 1 : 0.5

    Accessible.name: text
    Accessible.description: hint
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    contentItem: Item {
        Rectangle {
            id: box

            x: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 28
            height: 28
            radius: Theme.radius
            color: control.danger ? Theme.badSoft : Theme.accentSoft
            border.width: 1
            border.color: control.danger ? Qt.rgba(Theme.bad.r, Theme.bad.g, Theme.bad.b, 0.35) : Qt.rgba(Theme.accentText.r, Theme.accentText.g, Theme.accentText.b, 0.3)

            Icon {
                anchors.centerIn: parent
                size: 16
                name: control.symbol
                color: control.danger ? Theme.bad : Theme.accentText
            }
        }
        Text {
            textFormat: Text.PlainText
            anchors.left: box.right
            anchors.leftMargin: 10
            anchors.right: keys.visible ? keys.left : parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font: control.font
            color: Theme.text
            elide: Text.ElideRight
        }
        Rectangle {
            id: keys

            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            visible: control.hint !== ""
            width: keysText.implicitWidth + 12
            height: 20
            radius: 3
            color: Theme.inset
            border.width: 1
            border.color: Theme.line

            Text {
                id: keysText

                textFormat: Text.PlainText
                anchors.centerIn: parent
                text: control.hint
                color: Theme.muted
                font.pixelSize: 11
            }
        }
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.hovered || control.down ? Theme.hover : Theme.panel
        border.width: 1
        border.color: Theme.lineStrong

        Behavior on color {
            ColorAnimation {
                duration: Theme.fast
            }
        }

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
}
