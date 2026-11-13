import QtQuick
import QtQuick.Templates as T
import Qkvm

// A row of a panel that does something, as the page's ".menu-row" (app.css): its icon (`symbol`)
// and words at the left, and at the right the key that does the same (`key`), where there is one;
// the row's ground shows only under the pointer or the keyboard. Its keys as a row of Menu.qml.
T.Button {
    id: row

    property string symbol
    property string key

    implicitWidth: 9 + (symbol !== "" ? 24 : 0) + words.implicitWidth + 8 + (keyMark.visible ? keyMark.width + 14 : 0)
    implicitHeight: Theme.controlHeight
    font.family: Application.font.family
    font.pixelSize: Theme.textSize
    font.weight: Font.Medium
    Accessible.name: text
    Accessible.description: key
    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    contentItem: Item {
        Icon {
            id: sign

            x: 9
            anchors.verticalCenter: parent.verticalCenter
            visible: row.symbol !== ""
            name: row.symbol
            // (A picture of itself, a layer, as Check.qml says: a row may stand in what scrolls.)
            layer.enabled: true
        }
        Text {
            id: words

            textFormat: Text.PlainText
            x: sign.visible ? 33 : 9
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, parent.width - x - (keyMark.visible ? keyMark.width + 14 : 8))
            text: row.text
            color: Theme.text
            font: row.font
            elide: Text.ElideRight
        }
        // The key that does the same.
        Rectangle {
            id: keyMark

            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            visible: row.key !== ""
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
                text: row.key
                color: Theme.muted
                font.pixelSize: 11
            }
        }
    }
    background: Rectangle {
        radius: Theme.radius
        color: row.activeFocus || row.hovered ? Theme.hover : "transparent"
    }
}
