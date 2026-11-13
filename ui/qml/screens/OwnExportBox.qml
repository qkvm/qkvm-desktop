pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// This PC's own wall written to a file, by the program itself (ui/own_files_view.h): a passphrase
// for the file or none, then the file is made, and then every line of what is not in it stands
// in the box before "Save" asks, in Windows' own box, where the file is kept. There is no account
// here, and no password of one is asked for. Closed, the box keeps nothing: not what was typed,
// and not the file that was made. A file that is still being made when the box is closed is
// waited for no more, and is let go unseen when it is made (its making is not ended: it runs
// to its end, half a second or more, and another file waits for that).
//
// The box has one size and one place whatever it holds, as the box that takes a file in has: as
// large as its window has room for, up to its largest, in the middle of it. Its buttons stand at
// its foot; the lines of what is not in a file that was made fill the room between, and only
// they scroll. So nothing moves when the file is made, or when something is said in the box. Its
// size and its place are its window's: where the window changes size, moves, or comes to a
// screen of another scale, the box changes with it, and the program is told (`boxPlace`), which
// begins the moment of "Save" anew.
Dialog {
    id: box

    required property OwnFilesView files
    required property ActionsView actions
    // How high what the box holds is, at its one size: as high as the window has room for beside
    // the box's title and edges, with the box no higher than 480.
    readonly property real inside: Math.min(480, (parent ? parent.height : 600) - 24) - implicitHeaderHeight - topPadding - bottomPadding

    function send() {
        // (What is sent is on show no longer, as on the page.)
        passphrase.conceal();
        box.files.makeSelectedFile(passphrase.text, devices.checked, savedActions.checked, settings.checked);
    }

    title: Words.exportToAFile
    width: Math.min(520, parent ? parent.width - 24 : 520)
    // Not by a slip of the hand while the file is made, nor once it is made and not kept yet:
    // that is given up with the button that says so.
    closesBeside: !files.busy && !files.made
    onOpened: {
        devices.checked = true;
        savedActions.checked = true;
        settings.checked = true;
        passphrase.takeFocus();
    }
    onClosed: {
        passphrase.text = "";
        files.reset();
    }

    // The program is told whether what is not in the file is on show: the box is open on those
    // lines. Without that word nothing is saved.
    Binding {
        target: box.files
        property: "besideOnShow"
        value: box.opened && box.files.made && lines.visible
    }
    // And where the box stands on the screen, and how large it is, in the screen's own pixels,
    // with the place of its window on the screen counted in: while it is open.
    Binding {
        target: box.files
        property: "boxPlace"
        when: box.opened
        restoreMode: Binding.RestoreNone
        value: {
            const scale = whole.Screen.devicePixelRatio;
            const shownIn = whole.Window.window;
            const left = shownIn ? shownIn.x : 0;
            const top = shownIn ? shownIn.y : 0;
            return Qt.rect((left + box.x) * scale, (top + box.y) * scale, box.width * scale, box.height * scale);
        }
    }

    Item {
        id: whole

        width: box.availableWidth
        implicitHeight: box.inside

        Column {
            id: upper

            width: parent.width
            spacing: 14

            // First: a passphrase for the file, or none.
            Column {
                objectName: "ownExportAsked"
                width: parent.width
                visible: !box.files.made
                spacing: 4

                Check {
                    id: devices
                    objectName: "exportDevices"
                    text: qsTr("KVMs")
                    checked: true
                    enabled: !box.files.busy
                }
                Check {
                    id: savedActions
                    objectName: "exportActions"
                    text: qsTr("Saved actions")
                    checked: true
                    enabled: !box.files.busy
                }
                Check {
                    id: settings
                    objectName: "exportSettings"
                    text: qsTr("Wall settings: picture interval and update checks")
                    checked: true
                    enabled: !box.files.busy
                }

                Field {
                    id: passphrase

                    objectName: "ownExportPassphrase"
                    width: parent.width
                    // While the file is made, what it is made with is not written over.
                    enabled: !box.files.busy
                    label: Words.passphraseForTheFile
                    secret: true
                    onAccepted: box.send()
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: Words.whatAPassphraseDoesHere
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.WordWrap
                }
            }
            // Then: what was made.
            Text {
                textFormat: Text.PlainText
                objectName: "ownExportMade"
                width: parent.width
                visible: box.files.made
                text: box.files.madeEncrypted ? Words.theFileIsMadeEncrypted : Words.theFileIsMadePlain
                color: Theme.text
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
        }
        // And every line of what is not in it, none left out and none behind a click: as many as
        // there is room for, and the rest by scrolling.
        ListView {
            id: lines

            objectName: "ownExportNotInIt"
            anchors.top: upper.bottom
            anchors.topMargin: 14
            anchors.bottom: lower.visible ? lower.top : buttons.top
            anchors.bottomMargin: 14
            width: parent.width
            visible: box.files.made
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            spacing: 8
            model: box.files.notInIt

            delegate: Note {
                required property string modelData

                objectName: "ownExportLine"
                width: lines.width - 10
                tone: "warn"
                text: modelData
            }
        }
        ScrollMark {
            objectName: "ownExportScrollMark"
            flickable: lines
        }
        // Over the box's buttons: that the file is made; what went wrong; or what is to be known
        // and is nothing wrong (another file is being worked on just now). Where something is
        // said here, the lines end above it.
        Column {
            id: lower

            anchors.bottom: buttons.top
            anchors.bottomMargin: 14
            width: parent.width
            visible: box.files.making || box.files.problem !== "" || box.files.note !== ""
            spacing: 8

            Row {
                objectName: "ownExportWorking"
                visible: box.files.making
                spacing: 8

                Spinner {
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    text: Words.makingTheFile
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                }
            }
            Text {
                textFormat: Text.PlainText
                objectName: "ownExportProblem"
                width: parent.width
                visible: text !== ""
                text: box.files.problem
                color: Theme.bad
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
            Note {
                objectName: "ownExportNote"
                width: parent.width
                visible: text !== ""
                text: box.files.note
            }
        }
        // The box's buttons, at its foot, each in a place of its own that no other button ever
        // takes: no button comes to stand where another stood a moment before.
        Item {
            id: buttons

            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.controlHeight

            // The box's own button, at the right, in one place as wide as the wider of what it
            // says: "Make the file", and then "Save".
            Item {
                id: own

                anchors.right: parent.right
                width: Math.ceil(Math.max(makeWords.advanceWidth, saveWords.advanceWidth)) + 26
                height: parent.height

                TextMetrics {
                    id: makeWords

                    font.family: Application.font.family
                    font.pixelSize: Theme.textSize
                    font.weight: Font.Medium
                    text: Words.makeTheFile
                }
                TextMetrics {
                    id: saveWords

                    font.family: Application.font.family
                    font.pixelSize: Theme.textSize
                    font.weight: Font.Medium
                    text: Words.saveDots
                }
                Button {
                    objectName: "ownExportMake"
                    width: parent.width
                    visible: !box.files.made
                    primary: true
                    enabled: !box.files.busy && (devices.checked || savedActions.checked || settings.checked)
                    text: Words.makeTheFile
                    onClicked: box.send()
                }
                // Where "Make the file" stood: it takes no click for the first moment the lines
                // are on show (the program asks nothing then, and the button looks so).
                Button {
                    objectName: "ownExportSave"
                    width: parent.width
                    visible: box.files.made
                    primary: true
                    enabled: !box.files.busy && box.files.maySave
                    text: Words.saveDots
                    onClicked: box.files.saveFile()
                }
            }
            // (While the file is made the box closes over it, at once: the file is waited for no
            // more, and is nobody's when it is made.)
            Button {
                objectName: "ownExportCancel"
                anchors.right: own.left
                anchors.rightMargin: 8
                text: Words.cancel
                onClicked: box.close()
            }
        }
    }
    Connections {
        // The file is made: what it was made with is needed no more, and is out of its line.
        function onFileWasMade() {
            passphrase.text = "";
        }
        // It is kept: the box closes, and what was written is told in this window's corner.
        function onExported(said: string) {
            if (!box.opened)
                return;
            box.close();
            box.actions.tell(said, true);
        }
        // The program closes the box: its window is out of sight.
        function onCloseAsked() {
            box.close();
        }

        target: box.files
    }
}
