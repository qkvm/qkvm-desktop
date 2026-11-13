import QtQuick
import QtQuick.Templates as T
import Qkvm

// A box to tick, with what ticking it means beside it.
T.CheckBox {
    id: control

    implicitWidth: implicitContentWidth
    implicitHeight: Math.max(20, implicitContentHeight)
    spacing: 8
    // The program's own letters: a control would otherwise take those of Windows' theme.
    font.family: Application.font.family
    font.pixelSize: Theme.textSize
    Accessible.name: text

    indicator: Rectangle {
        y: Math.round((control.height - height) / 2)
        width: 16
        height: 16
        radius: 3
        color: control.checked ? Theme.accent : Theme.inset
        border.width: 1
        border.color: control.checked ? Theme.accentLine : Theme.lineStrong

        // The mark is drawn as a picture of itself (a layer). A box to tick may stand in a list,
        // which cuts off what is scrolled out over its edge. Where Qt Quick draws without a
        // graphics card (in software, as the tests' pictures are made), a drawing of lines, which
        // Icon is (a Shape), is not held to that edge: the mark of a box that was half scrolled
        // out stood over what lies beyond the list, in the picture. A picture of the mark is cut
        // off at the edge by either way of drawing. (ui/tests/test_own_files_screens.cpp looks at
        // the pixels beyond a list's edge.)
        Icon {
            anchors.centerIn: parent
            visible: control.checked
            name: "check"
            size: 12
            color: Theme.textOnAccent
            layer.enabled: true
        }
        // Where the keyboard is: the page's outline, two pixels out.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: 7
            color: "transparent"
            border.width: 2
            border.color: Theme.accentText
            visible: control.visualFocus
        }
    }
    contentItem: Text {
        textFormat: Text.PlainText
        leftPadding: 16 + control.spacing
        text: control.text
        font: control.font
        color: Theme.text
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.WordWrap
    }
}
