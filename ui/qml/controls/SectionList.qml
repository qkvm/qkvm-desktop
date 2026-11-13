pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import Qkvm

// The sections of the settings, the page's list of them: in their groups, a small heading over each
// where there are two or more (`groups`: [{title, sections: [{key, name, icon}]}]), each section with
// its icon; the one shown (`current`) is marked. Down the side; or, where the window is narrow
// (`inRow`), in one row above the section, which scrolls sideways and has no headings. Up and Down, and
// Left and Right, go from one to the next, round the ends, as in any list of tabs; Tab stops at the one
// shown. It says which was chosen (`chosen`), and shows what it is told.
Item {
    id: list

    property var groups: []
    property string current
    property bool inRow: false
    // The keys of the sections, in their order.
    readonly property var keys: groups.reduce((all, group) => all.concat(group.sections.map(one => one.key)), [])
    // The entries, one after the other: a heading where a group begins, then its sections.
    readonly property var entries: groups.reduce((all, group) => all.concat([
            {
                "heading": group.title,
                "first": all.length === 0
            }
        ], group.sections), [])
    // The button of each section, by its key.
    property var buttons: ({})

    // Which of the shapes the settings are made of this is (Section.qml).
    readonly property string shape: "sections"

    signal chosen(string key)

    // The next one, or the one before, is chosen, and has the keyboard and is in sight.
    function step(by: int) {
        if (keys.length === 0)
            return;
        const at = Math.max(0, keys.indexOf(current));
        // Move focus before the old tab's focus policy changes with current.
        list.forceActiveFocus(Qt.OtherFocusReason);
        chosen(keys[(at + by + keys.length) % keys.length]);
        takeKeyboard(true);
    }
    // The one shown has the keyboard, and is in sight. (Its outline shows where the keys brought it
    // there, and not where the box was only opened.)
    function takeKeyboard(byKeys: bool) {
        const button = buttons[current];
        if (button === undefined)
            return;
        button.forceActiveFocus(byKeys ? Qt.TabFocusReason : Qt.OtherFocusReason);
        const at = button.mapToItem(flick.contentItem, 0, 0);
        if (inRow) {
            if (at.x < flick.contentX)
                flick.contentX = Math.max(0, at.x - 10);
            else if (at.x + button.width > flick.contentX + flick.width)
                flick.contentX = Math.min(flick.contentWidth - flick.width, at.x + button.width - flick.width + 10);
        } else if (at.y < flick.contentY) {
            flick.contentY = Math.max(0, at.y - 14);
        } else if (at.y + button.height > flick.contentY + flick.height) {
            flick.contentY = Math.min(flick.contentHeight - flick.height, at.y + button.height - flick.height + 14);
        }
    }

    implicitHeight: inRow ? entriesGrid.height + 16 : entriesGrid.height + 28

    Flickable {
        id: flick

        anchors.fill: parent
        contentWidth: list.inRow ? entriesGrid.width + 20 : width
        contentHeight: list.inRow ? height : entriesGrid.height + 28
        flickableDirection: list.inRow ? Flickable.HorizontalFlick : Flickable.VerticalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Grid {
            id: entriesGrid

            x: 10
            y: list.inRow ? 8 : 14
            columns: list.inRow ? Math.max(1, list.entries.length) : 1
            rowSpacing: 2
            columnSpacing: 4

            Repeater {
                model: list.entries

                delegate: Item {
                    id: entry

                    required property var modelData
                    readonly property bool isHeading: modelData.heading !== undefined

                    visible: !isHeading || (!list.inRow && list.groups.length > 1)
                    width: list.inRow ? button.implicitWidth : list.width - 20
                    height: isHeading ? heading.height : 34

                    Text {
                        id: heading

                        textFormat: Text.PlainText
                        width: parent.width
                        visible: entry.isHeading
                        topPadding: entry.modelData.first === true ? 2 : 14
                        bottomPadding: 6
                        leftPadding: 10
                        text: entry.isHeading ? entry.modelData.heading : ""
                        color: Theme.muted
                        font.pixelSize: Theme.headingSize
                        font.weight: Font.DemiBold
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.9
                        elide: Text.ElideRight
                    }
                    T.AbstractButton {
                        id: button

                        readonly property string key: entry.isHeading ? "" : entry.modelData.key
                        readonly property bool isCurrent: key !== "" && key === list.current

                        objectName: "settingsSection"
                        anchors.fill: parent
                        visible: !entry.isHeading
                        implicitWidth: row.implicitWidth + 22
                        // Tab comes to the one shown, and the arrows go from there.
                        focusPolicy: isCurrent ? Qt.StrongFocus : Qt.ClickFocus
                        font.family: Application.font.family
                        font.pixelSize: Theme.textSize
                        font.weight: Font.Medium
                        Accessible.role: Accessible.PageTab
                        Accessible.name: entry.isHeading ? "" : entry.modelData.name
                        Accessible.selected: isCurrent
                        Keys.onUpPressed: list.step(-1)
                        Keys.onLeftPressed: list.step(-1)
                        Keys.onDownPressed: list.step(1)
                        Keys.onRightPressed: list.step(1)
                        onClicked: list.chosen(key)
                        Component.onCompleted: {
                            if (!entry.isHeading)
                                list.buttons[key] = button;
                        }

                        background: Rectangle {
                            radius: Theme.radius
                            color: button.isCurrent ? Theme.accentSoft : button.hovered ? Theme.hover : "transparent"
                            border.width: 1
                            border.color: button.isCurrent ? Qt.rgba(Theme.accentLine.r, Theme.accentLine.g, Theme.accentLine.b, Theme.accentLine.a * 0.45) : "transparent"

                            // Where the keyboard is: the page's outline, two pixels out.
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -3
                                radius: Theme.radius + 3
                                color: "transparent"
                                border.width: 2
                                border.color: Theme.accentText
                                visible: button.visualFocus
                            }
                        }
                        contentItem: Item {
                            Row {
                                id: row

                                x: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 10

                                Icon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: entry.isHeading ? "" : entry.modelData.icon
                                    size: 15
                                    color: button.isCurrent ? Theme.accentText : button.hovered ? Theme.text : Theme.muted
                                }
                                Text {
                                    textFormat: Text.PlainText
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: entry.isHeading ? "" : entry.modelData.name
                                    font: button.font
                                    color: button.isCurrent || button.hovered ? Theme.text : Theme.muted
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
