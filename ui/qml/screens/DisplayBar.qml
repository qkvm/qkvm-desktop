import QtQuick
import Qkvm

// The small bar of display mode, as the page has it: which group is shown, whether the groups take
// turns, and the way out. It shows for a moment when the pointer moves, and while the keyboard is in
// it, and gets out of the way again: the screens are what is shown.
Rectangle {
    id: bar

    required property WallView wall
    // How long it stays after the pointer last moved.
    property int staysMs: 3000
    property bool awake: false

    // (Where the keyboard was brought into it with Tab, and can be seen to be there.)
    readonly property bool shown: awake || rotate.visualFocus || leave.visualFocus
    // The group the wall is narrowed to: the one that is shown where the wall has it, and otherwise none.
    readonly property string group: wall.machines.groups.includes(wall.shown.group) ? wall.shown.group : ""

    // The pointer moved: said by whoever holds the window.
    function wake() {
        awake = true;
        sleep.restart();
    }

    implicitWidth: row.implicitWidth + 24
    implicitHeight: Theme.controlHeight + 14
    radius: Theme.radius + 2
    color: Theme.panel
    border.width: 1
    border.color: Theme.lineStrong
    opacity: shown ? 1 : 0
    Accessible.role: Accessible.ToolBar
    Accessible.name: Words.displayMode

    Behavior on opacity {
        NumberAnimation {
            duration: Theme.fast
        }
    }
    Timer {
        id: sleep

        interval: bar.staysMs
        onTriggered: bar.awake = false
    }

    // A click on it where it has no button is its own: a card under it is not clicked by it.
    MouseArea {
        anchors.fill: parent
    }
    Row {
        id: row

        anchors.centerIn: parent
        spacing: 12

        Text {
            textFormat: Text.PlainText
            objectName: "displayGroup"
            anchors.verticalCenter: parent.verticalCenter
            text: bar.group !== "" ? bar.group : Words.allGroups
            color: Theme.text
            font.pixelSize: Theme.textSize
            font.weight: Font.DemiBold
        }
        // Only where there is more than one group to take a turn.
        Check {
            id: rotate

            objectName: "displayRotate"
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.wall.machines.groups.length > 1
            text: Words.rotateGroups
            onToggled: bar.wall.rotating = checked

            Binding on checked {
                value: bar.wall.rotating
            }
        }
        Button {
            id: leave

            objectName: "displayExit"
            text: Words.exitDisplay
            onClicked: bar.wall.displaying = false
        }
    }
    // While it is out of the way, a tap where it lies brings it back and does nothing else: its
    // buttons are not pressed by somebody who cannot see them. (A pointer has brought it back by
    // moving, before it can click.)
    MouseArea {
        anchors.fill: parent
        visible: !bar.shown
        onPressed: bar.wake()
    }
}
