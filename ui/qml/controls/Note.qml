import QtQuick
import Qkvm

// A sentence set apart, as the page's notes are: on the inset colour, with a line down its left edge
// that says how serious it is ("warn", "bad", or nothing for something that is only worth knowing).
// With `code`, what it holds is shown letter for letter, in letters of one width: a text that will
// be typed, with its line ends where they are.
Rectangle {
    id: note

    property string text
    property string tone
    property bool code: false

    implicitHeight: words.implicitHeight + 18
    radius: Theme.radius
    color: Theme.inset
    border.width: 1
    border.color: Theme.line
    Accessible.role: Accessible.Note
    Accessible.name: text

    Rectangle {
        width: 3
        height: parent.height
        radius: Theme.radius
        color: note.tone === "warn" ? Theme.warn : note.tone === "bad" ? Theme.bad : Theme.accentText
    }
    Text {
        id: words

        textFormat: Text.PlainText
        x: 13
        y: 9
        width: parent.width - 25
        text: note.text
        color: Theme.text
        font.family: note.code ? "Consolas" : Application.font.family
        font.pixelSize: note.code ? Theme.smallSize : Theme.textSize
        // (Between words, and inside a word only where the word alone is longer than a line: a
        // name or an address in a sentence is never cut off at the note's edge.)
        wrapMode: note.code ? Text.WrapAnywhere : Text.Wrap
    }
}
