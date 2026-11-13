import QtQuick
import Qkvm

// The bar at the foot of the wall while the wall's own order is being made, as the page has it:
// what is being done and how, the way out, and "Save this order".
Rectangle {
    id: bar

    required property WallView wall

    implicitWidth: row.implicitWidth + 24
    implicitHeight: Theme.controlHeight + 16
    radius: Theme.radius + 2
    color: Theme.panel
    border.width: 1
    border.color: Theme.lineStrong
    Accessible.role: Accessible.ToolBar
    Accessible.name: Words.arrangeTheTiles

    // A press on it where it has no button is its own: a card under it is not dragged from there.
    MouseArea {
        anchors.fill: parent
    }
    Row {
        id: row

        anchors.centerIn: parent
        spacing: 8

        Text {
            textFormat: Text.PlainText
            anchors.verticalCenter: parent.verticalCenter
            text: Words.arrange
            color: Theme.text
            font.pixelSize: Theme.textSize
            font.weight: Font.DemiBold
        }
        Text {
            textFormat: Text.PlainText
            objectName: "arrangeHow"
            anchors.verticalCenter: parent.verticalCenter
            rightPadding: 6
            // (Where the window is too narrow for the sentence, the two buttons are what matters.)
            visible: bar.parent === null || implicitWidth + 320 < bar.parent.width
            text: Words.arrangeHow
            color: Theme.muted
            font.pixelSize: Theme.textSize
        }
        Button {
            objectName: "arrangeCancel"
            text: Words.cancel
            onClicked: bar.wall.stopArranging()
        }
        Button {
            objectName: "arrangeSave"
            primary: true
            text: Words.saveThisOrder
            enabled: !bar.wall.savingOrder
            onClicked: bar.wall.saveOrder()
        }
    }
}
