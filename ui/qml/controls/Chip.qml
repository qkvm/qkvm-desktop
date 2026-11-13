import QtQuick
import QtQuick.Templates as T
import Qkvm

// Something that is on or off, as the page's chips are: a word in a frame, filled while it is on.
// Whether it is on is said by whoever places it; a click, or Space, asks for the other.
T.Button {
    id: control

    property bool on: false

    implicitWidth: words.implicitWidth + 24
    implicitHeight: Theme.controlHeight
    font.family: Application.font.family
    font.pixelSize: Theme.textSize
    font.weight: Font.Medium
    Accessible.name: text
    Accessible.checkable: true
    Accessible.checked: on

    contentItem: Text {
        id: words

        textFormat: Text.PlainText
        text: control.text
        font: control.font
        color: control.on ? Theme.textOnAccent : Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: Theme.radius
        color: control.on ? Theme.accent : control.hovered ? Theme.hover : Theme.panel
        border.width: 1
        border.color: control.on ? Theme.accentLine : Theme.lineStrong

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
