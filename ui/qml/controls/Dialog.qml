import QtQuick
import QtQuick.Templates as T
import Qkvm

// A box over the window, as the page's dialogs are: its title with the way out at the right, a
// hairline, and what it holds. Esc and a click beside it close it, unless it says that they do
// not just now (`closesBeside`: something is under way that a slip of the hand is not to end).
// And where it says that it cannot be closed (`canClose`: something is under way that cannot be
// given up), the way out at the right is not offered either.
// A box may put something of its own into the line of its title, before the way out (`inTitle`):
// no other state of that box has a button at that place in the box. (In the window that place
// moves: a box stands in the middle of its window, so its title is higher up the higher the box
// is. Whoever puts a button there that must not take a click meant for another holds it back
// for a moment as well, as the box that changes a KVM does.)
T.Dialog {
    id: control

    property bool canClose: true
    property bool closesBeside: true
    property alias inTitle: besideTheTitle.data

    closePolicy: closesBeside ? T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside : T.Popup.NoAutoClose
    parent: T.Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    padding: 18

    implicitWidth: Math.max(implicitHeaderWidth, contentWidth + leftPadding + rightPadding)
    implicitHeight: implicitHeaderHeight + contentHeight + topPadding + bottomPadding

    background: Rectangle {
        color: Theme.panel
        radius: Theme.radius + 2
        border.width: 1
        border.color: Theme.line
    }

    header: Item {
        implicitHeight: 56

        Text {
            textFormat: Text.PlainText
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.right: besideTheTitle.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: control.title
            color: Theme.text
            font.pixelSize: Theme.titleSize
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Row {
            id: besideTheTitle

            anchors.right: closeButton.left
            // (Apart from the way out by more than a slip of the hand, where something is here.)
            anchors.rightMargin: width > 0 ? 16 : 0
            anchors.verticalCenter: parent.verticalCenter
        }
        Button {
            id: closeButton

            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            objectName: "closeBox"
            enabled: control.canClose
            ghost: true
            text: "✕"
            Accessible.name: Words.close
            onClicked: control.close()
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.line
        }
    }

    T.Overlay.modal: Rectangle {
        color: Theme.scrim
    }
}
