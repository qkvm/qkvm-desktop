import QtQuick
import QtQuick.Templates as T
import Qkvm

// One line of a frame of lines (Block), the page's "setting": what it is at the left (`label`, with a
// sentence under it where one is needed: `hint`), and what one does about it at the right: a tick, a
// choice, a button (what the line is given), or just what is so (`value`). Where the window is narrow,
// the right-hand side stands under the left. A line that says what went wrong has its words in the
// colour of a problem (`tone`: "bad").
//
// A line whose right-hand side is a tick takes a click on the whole of its left side as one on the
// tick, as the page's label does: what it says is what the tick is for.
Item {
    id: line

    property string label
    property string hint
    property string value
    property string tone
    // What one does about it, at its right: in the order given.
    default property alias does: doing.data

    // Which of the shapes the settings are made of this is (Section.qml): what a test walks them by.
    readonly property string shape: "line"
    readonly property bool narrow: Window.width > 0 && Window.width < Theme.narrowWindow
    readonly property bool doesSomething: doing.width > 0
    // The tick at its right, where it has one; null: none.
    readonly property T.CheckBox tick: {
        for (let at = 0; at < doing.children.length; ++at) {
            const one = doing.children[at];
            if (one instanceof T.CheckBox)
                return one as T.CheckBox;
        }
        return null;
    }

    width: parent ? parent.width : 0
    implicitHeight: narrow ? 26 + what.height + (doesSomething ? 8 + doing.height : 0) : Math.max(58, 26 + Math.max(what.height, doing.height))
    height: implicitHeight

    // Between it and the line before it in its frame.
    Rectangle {
        width: parent.width
        height: 1
        visible: line.Positioner.index > 0
        color: Theme.line
    }
    // Its left side, for the tick: all of it beside the right-hand side, or above it where that
    // stands under it. (Under what it says, which takes no click itself.)
    MouseArea {
        objectName: "lineLeft"
        y: 1
        width: line.narrow ? line.width : Math.max(0, doing.x - 14)
        height: (line.narrow ? Math.max(0, doing.y - 4) : line.height) - 1
        enabled: line.tick !== null && line.tick.enabled
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: line.tick.click()
    }
    Column {
        id: what

        x: 18
        y: line.narrow ? 13 : Math.round((line.height - height) / 2)
        width: line.narrow || !line.doesSomething ? line.width - 36 : line.width - 36 - 28 - doing.width
        spacing: 3

        Text {
            textFormat: Text.PlainText
            objectName: "lineLabel"
            width: parent.width
            visible: text !== ""
            text: line.label
            color: line.tone === "bad" ? Theme.bad : Theme.text
            font.pixelSize: Theme.textSize
            font.weight: Font.Medium
            wrapMode: Text.Wrap
        }
        Text {
            textFormat: Text.PlainText
            objectName: "lineHint"
            width: parent.width
            visible: text !== ""
            text: line.hint
            color: Theme.muted
            font.pixelSize: Theme.smallSize
            wrapMode: Text.Wrap
            lineHeight: 1.2
        }
    }
    Row {
        id: doing

        objectName: "lineDoes"
        x: line.narrow ? 18 : line.width - 18 - width
        y: line.narrow ? 13 + what.height + 8 : Math.round((line.height - height) / 2)
        spacing: 8

        // Just what is so: at the right, and whole.
        Text {
            textFormat: Text.PlainText
            objectName: "lineValue"
            width: Math.min(implicitWidth, line.narrow ? line.width - 36 : 420)
            visible: text !== ""
            text: line.value
            color: Theme.muted
            font.pixelSize: Theme.textSize
            horizontalAlignment: line.narrow ? Text.AlignLeft : Text.AlignRight
            wrapMode: Text.WrapAnywhere
        }
    }
}
