import QtQuick
import QtQuick.Templates as T
import Qkvm

// What something says whole where its place cuts some of it: a small note under it, a moment after
// the pointer came to rest there (whoever shows one says when). It takes no click and holds no
// keyboard, and goes with the pointer.
T.ToolTip {
    id: tip

    x: 18
    y: parent ? parent.height - 4 : 0
    margins: 8
    topPadding: 7
    bottomPadding: 7
    leftPadding: 10
    rightPadding: 10
    implicitWidth: Math.min(440, words.implicitWidth + leftPadding + rightPadding)
    implicitHeight: words.implicitHeight + topPadding + bottomPadding
    font.family: Application.font.family
    font.pixelSize: Theme.smallSize
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        id: words

        textFormat: Text.PlainText
        text: tip.text
        font: tip.font
        color: Theme.text
        wrapMode: Text.Wrap
        lineHeight: 1.2
    }
    background: Rectangle {
        radius: Theme.radius
        color: Theme.panel
        border.width: 1
        border.color: Theme.lineStrong
    }
}
