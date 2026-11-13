import QtQuick
import Qkvm

// What is said after the last block of a section of the settings (the page's "footnote"): under a
// hairline, in the quiet colour, no wider than is easy to read.
Column {
    id: note

    property string text

    // Which of the shapes the settings are made of this is (Section.qml).
    readonly property string shape: "footnote"

    width: parent ? parent.width : 0
    spacing: 16

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.line
    }
    Text {
        textFormat: Text.PlainText
        objectName: "footnoteText"
        width: Math.min(parent.width, 640)
        text: note.text
        color: Theme.muted
        font.pixelSize: Theme.textSize
        wrapMode: Text.Wrap
        lineHeight: 1.3
    }
}
