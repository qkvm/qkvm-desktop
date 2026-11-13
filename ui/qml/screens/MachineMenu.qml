pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// What can be done to one machine, as the page's menu for a machine has it: under the machine's
// name, what it is and how it is, and then the things to do, each kind under its heading. Which
// rows there are is the program's to say (ui/machine_menu.h): this draws what it is given, and says
// which row was chosen.
//
// It is opened and closed by the program. Closed by hand (Esc, the cross, a click beside it), the
// program is told. While something it sent is being answered or asked about, it holds still.
// Where it is longer than the window lets it be, it scrolls, a mark at its right edge says how far,
// and a row the keyboard comes to is brought into sight.
Dialog {
    id: box

    required property ActionsView actions

    readonly property var menu: actions.menu
    readonly property bool there: menu.title !== undefined

    function reveal(row: Item) {
        const top = row.mapToItem(all, 0, 0).y;
        if (top < scroller.contentY)
            scroller.contentY = Math.max(0, top - 8);
        else if (top + row.height > scroller.contentY + scroller.height)
            scroller.contentY = Math.min(scroller.contentHeight - scroller.height, top + row.height + 8 - scroller.height);
    }

    title: menu.title ?? ""
    width: Math.min(760, parent ? parent.width - 24 : 760)
    height: Math.min(implicitHeight, parent ? parent.height - 24 : implicitHeight)

    onThereChanged: {
        if (there)
            open();
        else
            close();
    }
    // Opened anew, it starts at its top, wherever it was left.
    onAboutToShow: scroller.contentY = 0
    onClosed: {
        if (there)
            actions.close();
    }

    contentItem: Flickable {
        id: scroller

        readonly property bool scrolls: contentHeight > height + 1

        implicitHeight: all.implicitHeight
        contentHeight: all.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        onContentHeightChanged: returnToBounds()

        Rectangle {
            objectName: "menuScrollMark"
            parent: scroller
            visible: scroller.scrolls
            x: scroller.width - width
            y: scroller.visibleArea.yPosition * scroller.height
            width: 3
            height: Math.max(24, scroller.visibleArea.heightRatio * scroller.height)
            radius: 1.5
            color: Theme.lineStrong
        }

        Column {
            id: all

            width: scroller.width - (scroller.scrolls ? 10 : 0)
            spacing: 14
            enabled: !box.actions.busy
            opacity: box.actions.busy ? 0.6 : 1

            Text {
                textFormat: Text.PlainText
                objectName: "menuAbout"
                width: parent.width
                visible: text !== ""
                text: box.menu.about ?? ""
                color: Theme.muted
                font.pixelSize: Theme.textSize
                wrapMode: Text.WrapAnywhere
            }
            Note {
                objectName: "menuWarning"
                width: parent.width
                visible: text !== ""
                tone: "warn"
                text: box.menu.warning ?? ""
            }
            // The battery of its Fingerbot, where that is low (as the page warns of it).
            Note {
                objectName: "menuBattery"
                width: parent.width
                visible: text !== ""
                tone: "warn"
                text: box.menu.battery ?? ""
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: text !== ""
                text: box.menu.kindNote ?? ""
                color: Theme.muted
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: box.menu.sections ?? []

                delegate: Column {
                    id: section

                    required property var modelData

                    width: all.width
                    spacing: 8

                    Text {
                        textFormat: Text.PlainText
                        visible: text !== ""
                        text: section.modelData.title
                        color: Theme.muted
                        font.pixelSize: Theme.headingSize
                        font.weight: Font.DemiBold
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.9
                    }
                    // As many in a row as are at least 230 wide, all of one width.
                    Flow {
                        id: grid

                        readonly property int columns: Math.max(1, Math.floor((width + spacing) / (230 + spacing)))
                        readonly property real cell: Math.floor((width - spacing * (columns - 1)) / columns)

                        width: parent.width
                        visible: section.modelData.rows.length > 0
                        spacing: 8

                        Repeater {
                            model: section.modelData.rows

                            delegate: ActionButton {
                                id: choice

                                required property var modelData

                                objectName: "row:" + modelData.id
                                width: grid.cell
                                symbol: modelData.icon
                                text: modelData.label
                                hint: modelData.hint
                                danger: modelData.danger
                                enabled: modelData.enabled
                                onClicked: box.actions.choose(modelData.id)
                                onActiveFocusChanged: {
                                    if (activeFocus)
                                        box.reveal(choice);
                                }
                            }
                        }
                    }
                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        visible: text !== ""
                        text: section.modelData.note
                        color: Theme.muted
                        font.pixelSize: Theme.smallSize
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
