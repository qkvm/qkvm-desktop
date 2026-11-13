import QtQuick
import Qkvm

// A block of a section of the settings, the page's "block": a small heading where it has one
// (`title`), and under it a frame of lines (Line), each with a hairline between it and the next. With
// `text` and no lines: a heading and what is to be read, without a frame (the page's block of text,
// as a licence is).
Column {
    id: block

    property string title
    property string text
    default property alias lines: inside.data

    // Which of the shapes the settings are made of this is (Section.qml).
    readonly property string shape: "block"

    width: parent ? parent.width : 0
    spacing: 10

    // The page's small heading: capitals, spaced, quiet.
    Text {
        textFormat: Text.PlainText
        objectName: "blockTitle"
        width: parent.width
        visible: block.title !== ""
        text: block.title
        color: Theme.muted
        font.pixelSize: Theme.headingSize
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.9
        elide: Text.ElideRight
    }
    Text {
        textFormat: Text.PlainText
        objectName: "blockText"
        width: Math.min(parent.width, 640)
        visible: block.text !== ""
        text: block.text
        color: Theme.text
        font.pixelSize: Theme.textSize
        wrapMode: Text.Wrap
        lineHeight: 1.3
    }
    Rectangle {
        width: parent.width
        height: inside.height + 2
        visible: block.text === ""
        radius: Theme.radius + 2
        color: Theme.lines
        border.width: 1
        border.color: Theme.line

        Column {
            id: inside

            objectName: "blockFrame"
            x: 1
            y: 1
            width: parent.width - 2
        }
    }
}
