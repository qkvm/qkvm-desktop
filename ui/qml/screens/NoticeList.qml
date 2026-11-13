pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import Qkvm

// The notices in the window's lower right corner, as the page has them: the newest lowest, each
// with a sign of its kind. They lie over everything, an open dialog too: what was just done from a
// menu is told while the menu is still there. So they live where the dialogs live, above them.
Column {
    id: list

    required property Notices notices
    // They lie over the wall's cards, and nothing else lies between: a click on a notice is then
    // the notice's, as on the page, and no card under it is clicked by it. Over a box, or over a
    // machine shown large, a click goes on to what is under the notice, as it always did here: a
    // box's buttons stay pressable while a notice of what was just done lies over them.
    property bool overTheWall: false

    parent: T.Overlay.overlay
    z: 1000
    x: parent ? parent.width - width - 16 : 0
    y: parent ? parent.height - height - 16 : 0
    width: parent ? Math.min(400, parent.width - 32) : 400
    spacing: 8

    Repeater {
        model: list.notices

        delegate: Rectangle {
            id: notice

            required property string text
            required property string kind
            // A certificate's SHA-256 in what is told stands on lines of its own, eight pairs
            // to each, as its box shows one: it has nothing a line could be broken at, and was
            // broken in the middle of a pair wherever the line ended.
            readonly property string drawn: text.replace(/ ?((?:[0-9A-F]{2}:){31}[0-9A-F]{2})(?![0-9A-F:])/g, (found, written) => "\n" + written.replace(/((?:[0-9A-F]{2}:){7}[0-9A-F]{2}):/g, "$1\n"))

            objectName: "notice"
            width: list.width
            height: Math.max(words.implicitHeight, 18) + 20
            radius: Theme.radius
            color: Theme.panel
            border.width: 1
            border.color: Theme.lineStrong
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text

            MouseArea {
                anchors.fill: parent
                enabled: list.overTheWall
            }
            Icon {
                x: 14
                y: 12
                visible: notice.kind !== "working"
                size: 16
                name: notice.kind === "ok" ? "check" : notice.kind === "error" ? "alert" : "info"
                color: notice.kind === "ok" ? Theme.ok : notice.kind === "error" ? Theme.bad : Theme.accentText
            }
            Spinner {
                x: 15
                y: 13
                visible: notice.kind === "working"
            }
            Text {
                id: words

                textFormat: Text.PlainText
                x: 40
                y: 10
                width: parent.width - 54
                text: notice.drawn
                color: Theme.text
                font.pixelSize: Theme.textSize
                // (Between words; and inside what has none to be broken at and is longer than
                // a line, which is shown whole all the same.)
                wrapMode: Text.Wrap
            }
        }
    }
}
