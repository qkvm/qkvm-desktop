import QtQuick
import Qkvm

// What explains a section of the settings, before its first block (the page's "intro"): to be read,
// in the quiet colour, no wider than is easy to read.
Text {
    // Which of the shapes the settings are made of this is (Section.qml).
    readonly property string shape: "intro"

    textFormat: Text.PlainText
    width: parent ? Math.min(parent.width, 640) : 0
    color: Theme.muted
    font.pixelSize: Theme.textSize
    wrapMode: Text.Wrap
    lineHeight: 1.3
}
