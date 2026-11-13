pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// The wall written to a file, as the page's "Export to a file" has it: what the file is to hold,
// a passphrase for it or none, and the password of whoever asks, once more. The server makes the
// file; where it is kept on this PC is asked in Windows' own box. Closed, it keeps nothing: not
// what was typed, and not what it had asked for, which is given up.
Dialog {
    id: box

    required property FilesView files
    required property ActionsView actions
    // What the window has room for, above the buttons: in a small window the rest is scrolled to.
    readonly property real room: Math.max(80, (parent ? parent.height : 600) - 24 - 56 - 36 - 14 - Theme.controlHeight - (problem.visible ? problem.height + 14 : 0))

    // One thing a file can hold, to be ticked.
    component Part: Check {
        required property var modelData
        readonly property string key: modelData.key

        objectName: "exportPart-" + modelData.key
        width: parent ? parent.width : 0
        text: modelData.label
        checked: true
    }

    // The line that has the keyboard is seen: in a low window it may lie below what the box shows.
    function show(line: Item) {
        const top = line.mapToItem(asked, 0, 0).y + asked.y;
        const bottom = top + line.height;
        if (top < list.contentY)
            list.contentY = Math.max(0, top - 6);
        else if (bottom > list.contentY + list.height)
            list.contentY = Math.min(list.contentHeight - list.height, bottom - list.height + 6);
    }

    function send() {
        // (What is sent is on show no longer, as on the page.)
        passphrase.conceal();
        own.conceal();
        box.files.exportWall(box.wanted(), passphrase.text, own.text);
    }
    function wanted() {
        const keys = [];
        for (let at = 0; at < ticks.count; at++) {
            const part = ticks.itemAt(at) as Part;
            if (part.checked)
                keys.push(part.key);
        }
        return keys;
    }

    title: Words.exportToAFile
    width: Math.min(520, parent ? parent.width - 24 : 520)
    // While the server works, a click beside the box does not end what was asked for.
    closesBeside: !files.busy
    onAboutToShow: {
        for (let at = 0; at < ticks.count; at++)
            (ticks.itemAt(at) as Part).checked = true;
    }
    onOpened: passphrase.takeFocus()
    onClosed: {
        passphrase.text = "";
        own.text = "";
        files.reset();
    }

    Column {
        width: box.availableWidth
        spacing: 14

        Flickable {
            id: list

            objectName: "exportAsked"
            width: parent.width
            height: Math.min(asked.height + 8, box.room)
            contentHeight: asked.height + 8
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // (Something said to be wrong takes room from it: the line being typed in stays seen.)
            onHeightChanged: {
                if (own.hasKeyboard)
                    box.show(own);
                else if (passphrase.hasKeyboard)
                    box.show(passphrase);
            }

            // (A little in from the edges: the line that has the keyboard is drawn with a rim.)
            Column {
                id: asked

                x: 4
                y: 4
                width: list.width - 14
                spacing: 14
                // While the server works, what it was asked is not written over.
                enabled: !box.files.busy

                Column {
                    width: parent.width
                    spacing: 4

                    Repeater {
                        id: ticks

                        model: box.files.partsOfAFile()

                        delegate: Part {}
                    }
                }
                Column {
                    width: parent.width
                    spacing: 4

                    Field {
                        id: passphrase

                        objectName: "exportPassphrase"
                        width: parent.width
                        label: Words.passphraseForTheFile
                        secret: true
                        onAccepted: own.takeFocus()
                        onHasKeyboardChanged: {
                            if (hasKeyboard)
                                box.show(passphrase);
                        }
                    }
                    Text {
                        textFormat: Text.PlainText
                        width: parent.width
                        text: Words.whatAPassphraseDoes
                        color: Theme.muted
                        font.pixelSize: Theme.smallSize
                        wrapMode: Text.WordWrap
                    }
                }
                Field {
                    id: own

                    objectName: "exportPassword"
                    width: parent.width
                    label: Words.yourPassword
                    secret: true
                    onAccepted: box.send()
                    onHasKeyboardChanged: {
                        if (hasKeyboard)
                            box.show(own);
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: Words.whatIsInNoFile
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.WordWrap
                }
            }
        }
        ScrollMark {
            objectName: "exportScrollMark"
            flickable: list
        }
        Text {
            id: problem

            textFormat: Text.PlainText
            objectName: "exportProblem"
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
                    objectName: "exportCancel"
                    text: Words.cancel
                    onClicked: box.close()
                }
                Button {
                    objectName: "exportSave"
                    primary: true
                    enabled: !box.files.busy
                    text: Words.saveDots
                    onClicked: box.send()
                }
            }
        }
    }
    // It is done: the box closes, and what came of it is told in this window's corner.
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
