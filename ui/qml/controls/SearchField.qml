import QtQuick
import QtQuick.Templates as T
import Qkvm

// "Find a KVM": a narrow field that widens while it is typed in or has something in it.
T.TextField {
    id: field

    property string hint

    implicitWidth: activeFocus || text !== "" ? 230 : 120
    implicitHeight: Theme.controlHeight
    leftPadding: 30
    rightPadding: 10
    verticalAlignment: TextInput.AlignVCenter
    color: Theme.text
    placeholderTextColor: Theme.muted
    selectionColor: Theme.accent
    selectedTextColor: Theme.textOnAccent
    font.family: Application.font.family
    font.pixelSize: Theme.textSize
    selectByMouse: true
    Accessible.name: hint

    Behavior on implicitWidth {
        NumberAnimation {
            duration: Theme.medium
            easing.type: Easing.OutCubic
        }
    }

    Icon {
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        name: "search"
        color: Theme.muted
    }
    Text {
        textFormat: Text.PlainText
        x: field.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        visible: field.text === "" && field.preeditText === ""
        text: field.placeholderText
        color: field.placeholderTextColor
        font: field.font
    }

    background: Rectangle {
        radius: Theme.radius
        color: Theme.inset
        border.width: 1
        border.color: field.activeFocus ? Theme.accentText : Theme.lineStrong

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: Theme.radius + 3
            color: "transparent"
            border.width: 3
            border.color: Theme.accentSoft
            visible: field.activeFocus
        }
    }
}
