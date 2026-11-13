import QtQuick
import Qkvm

// The bar at the foot of the wall while machines are being picked for something done to several of
// them, as the page has it: how many are picked, all that the bar's choices leave or none, what is
// done to them, and the way out.
Rectangle {
    id: bar

    required property WallView wall

    signal runAsked

    implicitWidth: row.implicitWidth + 24
    implicitHeight: Theme.controlHeight + 16
    radius: Theme.radius + 2
    color: Theme.panel
    border.width: 1
    border.color: Theme.lineStrong
    Accessible.role: Accessible.ToolBar
    Accessible.name: Words.bulkActions

    // A click on it where it has no button is its own: a card under it is not clicked by it.
    MouseArea {
        anchors.fill: parent
    }
    Row {
        id: row

        anchors.centerIn: parent
        spacing: 8

        Text {
            textFormat: Text.PlainText
            objectName: "pickedCount"
            anchors.verticalCenter: parent.verticalCenter
            rightPadding: 6
            text: Words.selected.arg(bar.wall.picked)
            color: Theme.text
            font.pixelSize: Theme.textSize
            font.weight: Font.DemiBold
        }
        Button {
            objectName: "pickAll"
            text: Words.selectAll
            onClicked: bar.wall.pickShown()
        }
        Button {
            objectName: "pickNone"
            text: Words.clear
            enabled: bar.wall.picked > 0
            onClicked: bar.wall.pickNone()
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: Theme.controlHeight - 8
            color: Theme.lineStrong
        }
        Button {
            objectName: "pickRun"
            primary: true
            symbol: "zap"
            text: Words.runAction
            enabled: bar.wall.picked > 0
            onClicked: bar.runAsked()
        }
        Button {
            objectName: "pickDone"
            ghost: true
            symbol: "x"
            hint: Words.leaveSelection
            onClicked: bar.wall.selecting = false
        }
    }
}
