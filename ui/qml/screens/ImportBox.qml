pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// A file taken in, as the page's "Import from a file" has it, step by step: a file is chosen in
// Windows' own box, the server reads it, and what it holds is said, each device and each action by
// itself, before the ticked parts are taken. Nothing on the wall is changed or removed by a file;
// what replaces a setting is ticked by whoever wants it, and not to begin with. Closed, the box
// keeps nothing: not the file, not what was typed. While the server takes a file in it cannot be
// closed: that cannot be given up, and what came of it is to be told.
Dialog {
    id: box

    required property FilesView files
    required property ActionsView actions
    // What the window has room for, for what a file holds, beside everything else in the box: in
    // a small window, or for a long file, the rest of it is scrolled to.
    readonly property real room: Math.max(40, (parent ? parent.height : 600) - 24 - 56 - 36 - choosing.height - 14 - (passphrase.visible ? passphrase.height + 14 : 0) - (problem.visible ? problem.height + 14 : 0) - 14 - Theme.controlHeight)

    // One part of what the file holds: to be ticked, with what it would add said under it, each
    // device and each action by itself.
    component Part: Column {
        id: one

        required property var modelData
        readonly property string key: modelData.key
        readonly property bool ticked: tick.checked && tick.enabled

        width: parent ? parent.width : 0
        spacing: 3

        Check {
            id: tick

            objectName: "importPart-" + one.modelData.key
            width: parent.width
            text: one.modelData.label
            enabled: one.modelData.can
            checked: one.modelData.ticked
        }
        Text {
            textFormat: Text.PlainText
            x: 24
            width: parent.width - x
            visible: text !== ""
            text: one.modelData.hint
            color: Theme.muted
            font.pixelSize: Theme.smallSize
            wrapMode: Text.WordWrap
        }
        Repeater {
            model: one.modelData.lines

            delegate: Column {
                id: entry

                required property var modelData
                property bool shown: false

                x: 24
                width: one.width - x
                spacing: 2

                Text {
                    textFormat: Text.PlainText
                    objectName: "importLine"
                    width: parent.width
                    text: entry.modelData.text
                    color: Theme.text
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.Wrap
                }
                // What a button will type is not taken unread.
                Button {
                    objectName: "importShowTyped"
                    visible: entry.modelData.typed !== ""
                    ghost: true
                    text: entry.shown ? Words.hideIt : Words.showWhatItTypes
                    onClicked: entry.shown = !entry.shown
                }
                Note {
                    objectName: "importTyped"
                    width: parent.width
                    visible: entry.shown
                    code: true
                    text: entry.modelData.typed
                }
            }
        }
    }

    function wanted() {
        const keys = [];
        for (let at = 0; at < found.count; at++) {
            const part = found.itemAt(at) as Part;
            if (part.ticked)
                keys.push(part.key);
        }
        return keys;
    }

    title: Words.importFromAFile
    width: Math.min(620, parent ? parent.width - 24 : 620)
    closesBeside: !files.busy
    canClose: !files.taking
    onClosed: {
        passphrase.text = "";
        files.reset();
    }

    Column {
        width: box.availableWidth
        spacing: 14

        Column {
            id: choosing

            width: parent.width
            spacing: 4

            Row {
                width: parent.width
                spacing: 10

                Button {
                    id: choose

                    objectName: "importChoose"
                    enabled: !box.files.busy
                    text: Words.chooseAFile
                    onClicked: box.files.chooseFile()
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "importFileName"
                    width: parent.width - choose.width - parent.spacing
                    height: choose.height
                    verticalAlignment: Text.AlignVCenter
                    text: box.files.fileName
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    elide: Text.ElideMiddle
                }
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                text: Words.aFileQkvmExported
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WordWrap
            }
        }
        Field {
            id: passphrase

            objectName: "importPassphrase"
            width: parent.width
            visible: box.files.sealed
            // While the server reads or takes, the passphrase it was handed is not written over.
            enabled: !box.files.busy
            label: Words.passphraseOfTheFile
            secret: true
            // What was read with one passphrase is not taken with another: it is read again.
            onTextChanged: box.files.readAnew()
            onAccepted: {
                if (!box.files.wasRead) {
                    passphrase.conceal();
                    box.files.readFile(passphrase.text);
                }
            }
        }

        // What the server read in the file: as much of it as the window has room for, and the rest
        // by scrolling.
        Flickable {
            id: list

            objectName: "importFound"
            width: parent.width
            height: Math.min(rows.height, box.room)
            contentHeight: rows.height
            visible: box.files.wasRead
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: rows

                width: list.width - 10
                spacing: 10

                Text {
                    textFormat: Text.PlainText
                    objectName: "importAbout"
                    width: parent.width
                    text: box.files.about
                    color: Theme.muted
                    font.pixelSize: Theme.textSize
                    wrapMode: Text.WordWrap
                }
                Note {
                    objectName: "importPlainSecrets"
                    width: parent.width
                    visible: box.files.plainSecrets
                    tone: "warn"
                    text: Words.thisFileHoldsPasswordsInPlainText
                }
                Repeater {
                    id: found

                    model: box.files.parts

                    delegate: Part {}
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "importNothing"
                    width: parent.width
                    visible: found.count === 0
                    text: Words.nothingInThisFileToTakeIn
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    wrapMode: Text.WordWrap
                }
                Note {
                    objectName: "importLeftOut"
                    width: parent.width
                    visible: text !== ""
                    tone: "warn"
                    text: box.files.leftOut
                }
            }
        }
        ScrollMark {
            objectName: "importScrollMark"
            flickable: list
        }
        Text {
            id: problem

            textFormat: Text.PlainText
            objectName: "importProblem"
            width: parent.width
            visible: text !== ""
            text: box.files.problem
            color: Theme.bad
            font.pixelSize: Theme.textSize
            wrapMode: Text.WordWrap
        }
        Item {
            width: parent.width
            height: Theme.controlHeight

            Row {
                anchors.right: parent.right
                spacing: 8

                Button {
                    objectName: "importCancel"
                    enabled: box.canClose
                    text: Words.cancel
                    onClicked: box.close()
                }
                Button {
                    objectName: "importRead"
                    visible: !box.files.wasRead
                    primary: true
                    enabled: !box.files.busy
                    text: Words.readTheFile
                    onClicked: {
                        passphrase.conceal();
                        box.files.readFile(passphrase.text);
                    }
                }
                Button {
                    objectName: "importTake"
                    visible: box.files.wasRead && box.files.canTake
                    primary: true
                    enabled: !box.files.busy
                    text: Words.takeIn
                    onClicked: {
                        passphrase.conceal();
                        box.files.takeIn(box.wanted(), passphrase.text);
                    }
                }
            }
        }
    }
    Connections {
        function onDone(said, good) {
            if (!box.opened)
                return;
            box.close();
            box.actions.tell(said, good);
        }

        target: box.files
    }
}
