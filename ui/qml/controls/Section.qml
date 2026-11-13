import QtQuick
import QtQuick.Templates as T
import Qkvm

// One section of the settings, in the one shape every section has (the page's `block` and
// `setting`). Its first line says what it is, its name and one short sentence (`name`, `about`), at
// the left, and has at its right what one does in the section as a whole (`does`: add, export); that
// line stays in sight while the rest scrolls under it. Under it comes the section's own, and nothing
// laid out otherwise: what explains it (Intro) before the first block, blocks of lines (Block), lists
// (Listed), and what is said after the last of them (Footnote); or a group of those that two places
// show alike (`shape` "blocks"). What explains never stands under a button.
//
// A section that asks for something it shows says so while it asks (`asking`), and what went wrong
// where that failed (`problem`): after the last of what it shows, where nothing stands that could be
// pushed away from under a pointer, and in the section itself, not in another's place. (What came to
// pass meanwhile in a box that a section opened is said by that box, which stays where it is.)
Item {
    id: section

    property string name
    property string about
    property string asking
    property string problem
    default property alias content: column.data
    property alias does: doing.data
    // What scrolls: all of the section but its first line.
    readonly property alias flickable: flick

    // Which of the shapes the settings are made of this is: every one of them says (Line, Block,
    // Listed, ListRow, Intro, Footnote), and a test walks them by it.
    readonly property string shape: "section"
    readonly property bool narrow: Window.width > 0 && Window.width < Theme.narrowWindow
    readonly property real pad: narrow ? 16 : 28

    // What has the keyboard, where Tab brought it there, is in sight. (Not where a click did: what
    // was clicked is in sight, and is not to move away from under the pointer.)
    function bringIntoSight(item: Item) {
        const control = item as T.Control;
        if (control === null || (control.focusReason !== Qt.TabFocusReason && control.focusReason !== Qt.BacktabFocusReason))
            return;
        let up = item;
        while (up !== null && up !== column)
            up = up.parent;
        if (up === null)
            return;
        const at = item.mapToItem(flick.contentItem, 0, 0).y;
        if (at < flick.contentY)
            flick.contentY = Math.max(0, at - 12);
        else if (at + item.height > flick.contentY + flick.height)
            flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, at + item.height - flick.height + 12));
    }

    clip: true

    Connections {
        function onActiveFocusItemChanged() {
            section.bringIntoSight(section.Window.activeFocusItem);
        }

        target: section.Window
    }

    // The first line.
    Rectangle {
        id: head

        objectName: "sectionHead"
        width: parent.width
        height: Math.max(section.narrow ? 0 : 69, what.height + (section.narrow ? 28 : 36), doing.height + 24)
        color: Theme.panel

        Item {
            id: what

            x: section.pad
            y: Math.round((head.height - height) / 2)
            width: head.width - 2 * section.pad - (doing.width > 0 ? doing.width + 24 : 0)
            height: told.beside ? named.height : named.height + 3 + told.height

            Text {
                id: named

                textFormat: Text.PlainText
                objectName: "sectionName"
                width: Math.min(implicitWidth, parent.width)
                text: section.name
                color: Theme.text
                font.pixelSize: 16
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            // The short sentence: beside the name where there is room for both, and under it
            // otherwise.
            Text {
                id: told

                readonly property bool beside: named.implicitWidth + 14 + aboutWidth.advanceWidth <= what.width

                textFormat: Text.PlainText
                objectName: "sectionTold"
                x: beside ? named.width + 14 : 0
                y: beside ? named.baselineOffset - baselineOffset : named.height + 3
                width: beside ? what.width - x : what.width
                text: section.about
                color: Theme.muted
                font.pixelSize: Theme.textSize
                wrapMode: Text.Wrap
            }
            TextMetrics {
                id: aboutWidth

                font.pixelSize: Theme.textSize
                text: section.about
            }
        }
        Row {
            id: doing

            objectName: "sectionDoes"
            x: head.width - section.pad - width
            y: Math.round((head.height - height) / 2)
            spacing: 8
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.line
        }
    }
    Flickable {
        id: flick

        objectName: "sectionContent"
        anchors.top: head.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: column.y + column.height + (said.visible ? 24 + said.height : 0) + section.pad
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: column

            x: section.pad
            y: section.narrow ? 18 : 24
            width: flick.width - 2 * section.pad
            spacing: 24
        }
        // What went wrong where it failed, or what is being asked: after the last of it.
        Note {
            id: said

            objectName: "sectionSaid"
            x: section.pad
            y: column.y + column.height + 24
            width: column.width
            visible: text !== ""
            text: section.problem !== "" ? section.problem : section.asking
            tone: section.problem !== "" ? "bad" : ""
        }
    }
    ScrollMark {
        flickable: flick
    }
}
