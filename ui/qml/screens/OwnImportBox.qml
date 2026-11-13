pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import Qkvm

// KVMs of a file taken onto this PC's own wall, by the program itself (ui/own_files_view.h), step
// by step: a file is chosen in Windows' own box, it is read, and what it holds is laid out with
// nothing on the wall changed. Above the list, and not scrolled away with it, stands once what is
// so of all of it: how many of the file's devices are in the list, how many are ticked, how many
// want a look, how many cannot be taken, what is so of every one alike, and that each KVM that is
// taken is asked what it is before a password goes to it. In the list: first the lines that stand
// beside the devices (folded into one row until they are opened), then every device of the file
// that is in it, each one short row: its tick, its name and address, and the one thing that
// matters of it. The rest of it is behind "Details". Only what is ticked is taken, and the ticks
// are the program's: a row that was scrolled away comes back as it was. Where every one that was
// ticked is on the wall the box closes; where one is not, it stays and says what came of each.
// Closed, the box keeps nothing: not the file, not what was laid out, not what was typed.
//
// The box has one size and one place whatever it holds: as large as its window has room for, up
// to its largest, in the middle of it. Its buttons stand at its foot, each in a place of its own;
// what stands over the list keeps the room of the highest it can say; and only a list scrolls
// inside the box. So nothing that is pressed comes to stand where something else stood a moment
// before because the box grew or shrank, and what changes in the list changes nothing outside it.
// Its size and its place are its window's: where the window changes size, moves, or comes to a
// screen of another scale, the box changes with it, and the program is told (`boxPlace`), which
// begins the moments of the list and of what takes anew.
Dialog {
    id: box

    required property OwnFilesView files
    required property ActionsView actions
    // How high what the box holds is, at its one size: as high as the window has room for beside
    // the box's title and edges, with the box no higher than 760.
    readonly property real inside: Math.min(760, (parent ? parent.height : 600) - 24) - implicitHeaderHeight - topPadding - bottomPadding

    function read() {
        // (What is sent is on show no longer, as on the page.)
        passphrase.conceal();
        box.files.readFile(passphrase.text);
    }
    // The list scrolled by so much, after a row was opened or closed: so much did that row's
    // first line move in what the list shows (it grew or shrank above that line; or the list's
    // end came up to meet a row that shrank, and pulled every row down). Where the list's end is
    // too near for that, room is left after its end, which goes as the list is scrolled back.
    function scrollBy(by: real) {
        if (Math.abs(by) < 0.5)
            return;
        const to = list.contentY + by;
        list.bottomMargin = Math.max(list.bottomMargin, to + list.height - list.originY - list.contentHeight);
        list.contentY = Math.max(list.originY, to);
    }

    title: Words.importFromAFile
    width: Math.min(700, parent ? parent.width - 24 : 700)
    // Not by a slip of the hand while a file is read, nor while what it holds is laid out with
    // its ticks: the box is left with the button that says so. Nor while what came of a taking is
    // shown, nor once the list has gone by itself: what is said there is to be read, and a click
    // that lands beside the box closes nothing.
    closesBeside: !files.busy && !files.laidOut && !files.took && !files.keptNoLonger
    onClosed: {
        passphrase.text = "";
        files.reset();
    }

    // The program is told whether the list is looked at: the box is open on it, in a window that
    // is the active one (one that is out of sight, or made small, is not). The moment after which
    // something is taken runs only then, and from the beginning each time.
    Binding {
        target: box.files
        property: "lookedAt"
        value: box.opened && box.files.laidOut && list.Window.active
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
    // A taking took every device that was ticked, and the box has gone from under the pointer
    // that pressed "Take": what the window shows there takes no press for a moment. Nothing is
    // drawn for it: it only takes every press, over the whole window, until then.
    T.Popup {
        objectName: "ownImportHeldAfterTaking"
        parent: T.Overlay.overlay
        x: 0
        y: 0
        width: parent ? parent.width : 0
        height: parent ? parent.height : 0
        padding: 0
        modal: true
        dim: false
        focus: false
        closePolicy: T.Popup.NoAutoClose
        visible: box.files.heldAfterTaking
        background: Item {}
        contentItem: Item {}
    }

    Item {
        id: whole

        readonly property bool windowShown: !Window.window || (Window.window.visible && Window.window.visibility !== Window.Minimized)

        width: box.availableWidth
        implicitHeight: box.inside

        // Which file, at the top. (Not once a taking is over: then nothing is left to choose,
        // and its place stays empty.)
        Row {
            id: choosing

            width: parent.width
            height: Theme.controlHeight
            visible: !box.files.took
            spacing: 10

            Button {
                id: choose

                objectName: "ownImportChoose"
                enabled: !box.files.busy
                text: Words.chooseAFile
                onClicked: box.files.chooseFile()
            }
            // The file that was chosen, by its name; and until one is, what a file is to be.
            Text {
                textFormat: Text.PlainText
                objectName: "ownImportFileName"
                width: parent.width - choose.width - parent.spacing
                height: choose.height
                verticalAlignment: Text.AlignVCenter
                text: box.files.fileName !== "" ? box.files.fileName : Words.aFileQkvmExported
                color: box.files.fileName !== "" ? Theme.text : Theme.muted
                font.pixelSize: box.files.fileName !== "" ? Theme.textSize : Theme.smallSize
                elide: Text.ElideMiddle
            }
        }
        // Between the button that chooses and the list: before a list, the line for a file's
        // passphrase; with a list, what stands over it. As high as the higher of the two, the
        // second with the room of the highest it can say with this list, as that is drawn at this
        // width: so the line that is pressed before a list comes stands where nothing of a list
        // stands, and the list's top stays where it is whatever is ticked. (It moves where the
        // box's width does, and the box is held for that.)
        Item {
            id: band

            anchors.top: choosing.bottom
            anchors.topMargin: 14
            width: parent.width
            height: Math.max(passphrase.implicitHeight, countsRoom.highest + 4 + nothingYet.implicitHeight)

            // Its passphrase: asked for only where the file says of itself that it is sealed, and
            // only until it was read (nothing needs it twice). What was typed is that file's,
            // and stays in the line for no longer than the line is in sight: the line goes when
            // its file is read, when its file is not the chosen one any more (another was
            // chosen, sealed or plain; it was refused for good) and when the box is closed, and
            // is emptied at each of these (below, and onClosed).
            Field {
                id: passphrase

                objectName: "ownImportPassphrase"
                width: parent.width
                visible: box.files.sealed && !box.files.laidOut && !box.files.took
                // While the file is read, the passphrase it is read with is not written over.
                enabled: !box.files.busy
                label: Words.passphraseOfTheFile
                secret: true
                onAccepted: box.read()
            }
            // What stands over the list, once, and is not scrolled away with it: how many of the
            // file's devices are in the list, how many of them are ticked, how many are not and
            // want a look, how many cannot be taken; and that nothing has changed yet, nor was
            // anything asked at any address. Or, once a taking is over and not every one is on
            // the wall, how many are.
            Column {
                id: above

                objectName: "ownImportAbove"
                width: parent.width
                visible: box.files.laidOut || box.files.took
                spacing: 4

                // (With the room of the highest it can say: a tick that makes it shorter by a
                // line, or longer, moves nothing under it.)
                Text {
                    id: counts

                    textFormat: Text.PlainText
                    objectName: "ownImportCounts"
                    width: parent.width
                    height: Math.max(implicitHeight, countsRoom.highest)
                    text: box.files.took ? box.files.tookSummary : box.files.counts
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    wrapMode: Text.WordWrap
                }
                Text {
                    id: nothingYet

                    textFormat: Text.PlainText
                    objectName: "ownImportNothingYet"
                    width: parent.width
                    visible: box.files.laidOut
                    text: box.files.hasOtherParts ? qsTr("Nothing has changed yet. Only selected items will be imported.") : Words.nothingOnTheWallHasChangedYet
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.WordWrap
                }
            }
            // How high the highest is of what can stand over the list with this list: every form
            // of it that may take the most room (the program's), each drawn as the counts are, in
            // their letters and at their width, and never shown. The program is told how wide each
            // digit is drawn in those letters, to find the widest of the numbers.
            Item {
                id: countsRoom

                readonly property real highest: {
                    let most = 0;
                    for (let at = 0; at < forms.count; ++at) {
                        const form = forms.itemAt(at);
                        if (form)
                            most = Math.max(most, form.implicitHeight);
                    }
                    return most;
                }

                objectName: "ownImportCountsRoom"
                width: parent.width
                visible: false

                Repeater {
                    id: forms

                    model: box.files.countsToMakeRoomFor

                    delegate: Text {
                        required property string modelData

                        textFormat: Text.PlainText
                        width: countsRoom.width
                        text: modelData
                        font: counts.font
                        wrapMode: Text.WordWrap
                    }
                }
                FontMetrics {
                    id: digits

                    font: counts.font
                }
                Binding {
                    target: box.files
                    property: "digitWidths"
                    // (Measured again whenever the letters are others.)
                    value: digits.font.pixelSize > 0 ? [0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map(digit => digits.advanceWidth(String(digit))) : []
                }
            }
        }
        // What the file holds: as much of it as there is room for between what stands over it
        // and the box's foot, and the rest by scrolling. Rows are made as they come into sight,
        // and not one for every device.
        ListView {
            id: list

            objectName: "ownImportOffered"
            anchors.top: band.bottom
            anchors.topMargin: 14
            anchors.bottom: lower.visible ? lower.top : buttons.top
            anchors.bottomMargin: 14
            width: parent.width
            visible: box.files.laidOut
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            spacing: 10
            // (None ahead of time: a row is made, whole, at the moment it comes into sight.
            // Rows that a list makes ahead it makes bit by bit, and one that is half made when
            // the list is emptied has its sentences taken from under it, which Qt complains of.)
            cacheBuffer: 0
            // The keys scroll what is seen, a step or a page at a time. (Not a row of the
            // keyboard's own that nobody sees, which the list would keep for the next file and
            // show that one from: a new list begins at its top.)
            keyNavigationEnabled: false
            model: box.files.offered
            // Room that was left after the list's end for a row that shrank (box.scrollBy) goes
            // as the list is scrolled back from it.
            onContentYChanged: {
                if (bottomMargin > 0)
                    bottomMargin = Math.max(0, Math.min(bottomMargin, contentY + height - originY - contentHeight));
            }
            // The keys move the list the way they say, and never past what is there: down no
            // further than its end, or than the room that is left after it, and up no further than
            // its top. (To either end as the list itself goes there: where the rows that are not
            // made yet are only reckoned, it makes those at that end first, and stands at the end
            // of them.) Where they moved it, the rows under a pointer that rests where it rested
            // are others at once, and the program holds what is in the list for its moment.
            Keys.onPressed: event => {
                const stood = list.contentY;
                if (event.key === Qt.Key_End || event.key === Qt.Key_Home) {
                    if (event.key === Qt.Key_End)
                        list.positionViewAtEnd();
                    else
                        list.positionViewAtBeginning();
                } else {
                    const page = Math.max(40, list.height - 40);
                    const by = event.key === Qt.Key_Down ? 40 : event.key === Qt.Key_Up ? -40 : event.key === Qt.Key_PageDown ? page : event.key === Qt.Key_PageUp ? -page : 0;
                    if (by === 0)
                        return;
                    const most = Math.max(list.originY, list.originY + list.contentHeight + list.bottomMargin - list.height);
                    list.contentY = Math.max(list.originY, Math.min(most, list.contentY + by));
                }
                event.accepted = true;
                if (Math.abs(list.contentY - stood) >= 0.5)
                    box.files.rowsMoved();
            }

            // One row: a line that stands beside the devices, the row that stands for such lines
            // where they are folded away, or a device.
            delegate: Item {
                id: row

                required property string kind
                required property string text
                required property int place
                required property string nameAndAddress
                required property string inShort
                required property bool wantsALook
                required property string verdict
                required property list<string> says
                required property string fingerprint
                required property bool canBeTaken
                required property bool ticked
                required property bool open
                required property bool hasText

                readonly property bool isDevice: kind === "device" || kind === "action" || kind === "settings"
                property string revealedText: ""
                onOpenChanged: {
                    if (!open)
                        revealedText = "";
                }

                Connections {
                    target: whole
                    function onWindowShownChanged() {
                        if (!whole.windowShown)
                            row.revealedText = "";
                    }
                }

                // Opens what stands behind the row, or closes it, with the row's first line where
                // it stood in what the list shows. (Laid out at once and not at the next frame:
                // where the row is now is looked at here.)
                function openOrClose() {
                    const stood = row.y - list.contentY;
                    box.files.setOpen(row.place, !row.open);
                    behind.forceLayout();
                    told.forceLayout();
                    list.forceLayout();
                    box.scrollBy(row.y - list.contentY - stood);
                }

                objectName: "ownImportRow"
                width: list.width - 10
                height: isDevice ? device.height : kind === "fold" ? fold.height : beside.height

                Note {
                    id: beside

                    objectName: "ownImportBeside"
                    width: parent.width
                    visible: row.kind === "beside"
                    tone: "warn"
                    text: row.text
                }
                // The lines beside the devices, folded into one row however many and however long
                // they are: how many there are, opened by a click. Nothing is taken before they
                // were.
                Item {
                    id: fold

                    objectName: "ownImportFold"
                    width: parent.width
                    height: Theme.controlHeight
                    visible: row.kind === "fold"

                    Button {
                        id: more

                        objectName: "ownImportMoreNotes"
                        // (Nothing of the list takes a click until it has stood as it is for its
                        // moment: what comes with it, or moves when its notes are opened or a row
                        // of it is, may stand where something else stood a moment before. The
                        // program holds to that too, whoever presses.)
                        enabled: !box.files.busy && box.files.settled
                        noted: true
                        text: row.text
                        onClicked: box.files.openNotes()
                    }
                    Text {
                        textFormat: Text.PlainText
                        anchors.left: more.right
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: Words.toBeOpenedBeforeAnythingIsTaken
                        color: Theme.muted
                        font.pixelSize: Theme.smallSize
                        wrapMode: Text.WordWrap
                    }
                }
                Item {
                    id: device

                    // How much of the row's top has gone out over the list's upper edge.
                    readonly property real cut: Math.max(0, list.contentY - row.y)
                    // A row that is opened (it may be higher than the list) whose top has gone
                    // out so: its tick stands at that edge, and the device's name and address
                    // with it.
                    readonly property bool stuck: row.open && cut > 0

                    objectName: "ownImportDevice"
                    width: parent.width
                    height: Math.max(Theme.controlHeight, told.height)
                    visible: row.isDevice

                    Column {
                        id: told

                        x: 28
                        width: parent.width - x
                        spacing: 2

                        // Its name and its address in one piece, whole: broken into lines
                        // wherever it has to be, and never cut short. A long name does not push
                        // the address out of sight.
                        Text {
                            textFormat: Text.PlainText
                            objectName: "ownImportNameAndAddress"
                            width: parent.width - details.width - 8
                            text: row.nameAndAddress
                            color: row.canBeTaken ? Theme.text : Theme.muted
                            font.pixelSize: Theme.textSize
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                        // The one thing that matters of it, in a few words. Where that is why it
                        // is not ticked to begin with, in the colour of a warning: it is a
                        // question to whoever reads it.
                        Text {
                            textFormat: Text.PlainText
                            objectName: "ownImportInShort"
                            width: parent.width - details.width - 8
                            text: row.inShort
                            color: row.wantsALook ? Theme.warn : Theme.muted
                            font.pixelSize: Theme.smallSize
                            wrapMode: Text.Wrap
                        }
                        // Behind "Details": whether it can be taken and as what, or why not, as
                        // a sentence; everything else that is said of it, every sentence, each by
                        // itself; and the certificate the file names for it, whole.
                        Column {
                            id: behind

                            objectName: "ownImportBehind"
                            width: parent.width
                            visible: row.open
                            topPadding: 4
                            spacing: 3

                            Text {
                                textFormat: Text.PlainText
                                objectName: "ownImportVerdict"
                                width: parent.width
                                text: row.verdict
                                color: row.canBeTaken ? Theme.text : Theme.muted
                                font.pixelSize: Theme.smallSize
                                wrapMode: Text.Wrap
                            }
                            Repeater {
                                model: row.says

                                delegate: Text {
                                    required property string modelData

                                    textFormat: Text.PlainText
                                    objectName: "ownImportSays"
                                    width: told.width
                                    text: modelData
                                    color: Theme.muted
                                    font.pixelSize: Theme.smallSize
                                    wrapMode: Text.Wrap
                                }
                            }
                            Button {
                                objectName: "importShowText"
                                visible: row.kind === "action" && row.canBeTaken && row.hasText
                                enabled: !box.files.busy && whole.windowShown
                                text: row.revealedText === "" ? qsTr("Show text") : qsTr("Hide text")
                                onClicked: {
                                    row.revealedText = row.revealedText === "" ? box.files.actionText(row.place) : "";
                                    box.files.rowsMoved();
                                }
                            }
                            Text {
                                objectName: "importActionText"
                                textFormat: Text.PlainText
                                visible: row.revealedText !== ""
                                width: parent.width
                                text: row.revealedText
                                color: Theme.text
                                font.pixelSize: Theme.smallSize
                                wrapMode: Text.Wrap
                            }
                            // In the letters the box that changes a KVM shows one in: four lines
                            // of eight, to be compared by eye.
                            Text {
                                textFormat: Text.PlainText
                                width: parent.width
                                visible: row.fingerprint !== ""
                                topPadding: 3
                                text: Words.sha256OfTheCertificateTheFileNames
                                color: Theme.muted
                                font.pixelSize: Theme.smallSize
                                wrapMode: Text.Wrap
                            }
                            Text {
                                textFormat: Text.PlainText
                                objectName: "ownImportFingerprint"
                                visible: row.fingerprint !== ""
                                text: row.fingerprint
                                color: Theme.text
                                font.family: "Consolas"
                                font.pixelSize: Theme.textSize
                                lineHeight: 1.25
                            }
                        }
                    }
                    // Where the row's own line with its name has gone out over the list's upper
                    // edge, its name and address stand at that edge, beside its tick, whole, on
                    // the box's own colour across the row (what is scrolled under them is not
                    // seen through them), until the row's end comes: what is ticked there and
                    // what is read below it are of the one device. (What is left in sight of
                    // "Details" stands over it.)
                    Rectangle {
                        id: stuckName

                        objectName: "ownImportStuckName"
                        y: Math.round(Math.min(device.height - height, device.cut))
                        width: device.width
                        height: Math.max(tick.height, stuckWords.height) + 6
                        visible: device.stuck
                        color: Theme.panel

                        Text {
                            id: stuckWords

                            textFormat: Text.PlainText
                            objectName: "ownImportStuckNameText"
                            x: told.x
                            width: details.x - 8 - x
                            text: row.nameAndAddress
                            color: row.canBeTaken ? Theme.text : Theme.muted
                            font.pixelSize: Theme.textSize
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                    }
                    // To be had only where the device can be taken. What is ticked is the
                    // program's to keep: the tick shows it, and a click asks for another. In
                    // sight for as long as any of its row is: where the row's top has gone out
                    // over the list's upper edge, the tick stays at that edge until the row's end
                    // comes (a row that is opened may be higher than the list). What is ticked
                    // and is taken is never out of sight beside what is read of it.
                    Check {
                        id: tick

                        objectName: "ownImportTick"
                        y: Math.round(Math.max(0, Math.min(device.height - height, device.cut)))
                        width: 22
                        height: 22
                        visible: row.canBeTaken
                        enabled: !box.files.busy && box.files.settled
                        checked: row.ticked
                        Accessible.name: row.nameAndAddress
                        onToggled: {
                            box.files.setTicked(row.place, checked);
                            checked = Qt.binding(() => row.ticked);
                        }
                    }
                    // One that cannot be taken has no tick, and nothing to press: a short bar
                    // stands where one would be. It is over, and looks so: grey, where one that
                    // can be taken and is not ticked asks to be looked at.
                    Rectangle {
                        objectName: "ownImportNoTick"
                        x: 3
                        y: 10
                        width: 10
                        height: 2
                        radius: 1
                        visible: !row.canBeTaken
                        color: Theme.lineStrong
                    }
                    // Opens what stands behind the row, and closes it. (What is open is the
                    // program's to keep, as a tick is: a row that was scrolled away comes back as
                    // it was.)
                    Button {
                        id: details

                        objectName: "ownImportDetails"
                        anchors.right: parent.right
                        enabled: !box.files.busy && box.files.settled
                        ghost: true
                        checked: row.open
                        text: Words.details
                        onClicked: row.openOrClose()
                    }
                }
            }
        }
        // Where there is more than the list shows: below it, or above it. (A list that has room
        // left after its end, kept for a row that shrank, may fit whole and still stand scrolled,
        // with its first row in part above its edge: its mark is there as long as it does.)
        ScrollMark {
            objectName: "ownImportScrollMark"
            visible: list.contentHeight > list.height + 1 || list.contentY > list.originY + 0.5
            flickable: list
        }
        // What came of each that was ticked and is not on the wall: what it is, and why not. In
        // the list's place, as large.
        ListView {
            id: untaken

            objectName: "ownImportNotTaken"
            anchors.top: band.bottom
            anchors.topMargin: 14
            anchors.bottom: lower.visible ? lower.top : buttons.top
            anchors.bottomMargin: 14
            width: parent.width
            visible: box.files.took
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            spacing: 10
            model: box.files.notTaken

            header: Text {
                textFormat: Text.PlainText
                width: untaken.width - 10
                bottomPadding: 8
                text: Words.notTakenColon
                color: Theme.muted
                font.pixelSize: Theme.textSize
            }
            delegate: Column {
                id: one

                required property var modelData

                objectName: "ownImportLeft"
                width: untaken.width - 10
                spacing: 3

                Text {
                    textFormat: Text.PlainText
                    objectName: "ownImportLeftDevice"
                    width: parent.width
                    text: one.modelData.device
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "ownImportLeftWhy"
                    width: parent.width
                    text: one.modelData.why
                    color: Theme.warn
                    font.pixelSize: Theme.textSize
                    wrapMode: Text.Wrap
                }
            }
        }
        ScrollMark {
            objectName: "ownImportLeftScrollMark"
            flickable: untaken
        }
        // Over the box's buttons: that the file is read; what went wrong; or what is to be known
        // and is nothing wrong (another file is being worked on just now; what was read is kept
        // no longer). Where something is said here, a list ends above it.
        Column {
            id: lower

            anchors.bottom: buttons.top
            anchors.bottomMargin: 14
            width: parent.width
            visible: box.files.reading || box.files.problem !== "" || box.files.note !== ""
            spacing: 8

            Row {
                objectName: "ownImportWorking"
                visible: box.files.reading
                spacing: 8

                Spinner {
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    text: Words.readingTheFile
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                }
            }
            Text {
                textFormat: Text.PlainText
                objectName: "ownImportProblem"
                width: parent.width
                visible: text !== ""
                text: box.files.problem
                color: Theme.bad
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
            Note {
                objectName: "ownImportNote"
                width: parent.width
                visible: text !== ""
                text: box.files.note
            }
        }
        // The box's buttons, at its foot, each in a place of its own that no other button ever
        // takes: no button comes to stand where another stood a moment before, whatever the box
        // holds.
        Item {
            id: buttons

            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.controlHeight

            // The box's own button, at the right, in one place as wide as the widest it can say:
            // "Read the file" until the list is shown, and then the button that takes, for as
            // long as the list is shown. (Once a taking is over, the place stays, with nothing in
            // it; and "Close" stands where "Cancel" stood, as the box is where it was.)
            Item {
                id: own

                anchors.right: parent.right
                width: Math.ceil(Math.max(readWords.advanceWidth, takeWords.advanceWidth)) + 26
                height: parent.height

                TextMetrics {
                    id: readWords

                    font.family: Application.font.family
                    font.pixelSize: Theme.textSize
                    font.weight: Font.Medium
                    text: Words.readTheFile
                }
                // (As wide as for more devices than a file may hold: its width is not another
                // when the number on it is.)
                TextMetrics {
                    id: takeWords

                    font.family: Application.font.family
                    font.pixelSize: Theme.textSize
                    font.weight: Font.Medium
                    text: Words.takeKvms.arg("00000")
                }
                Button {
                    objectName: "ownImportRead"
                    width: parent.width
                    visible: !box.files.laidOut && !box.files.took
                    primary: true
                    enabled: !box.files.busy && box.files.fileName !== ""
                    text: Words.readTheFile
                    onClicked: box.read()
                }
                // Where "Read the file" stood, and there for as long as the list is shown. It
                // takes no click while nothing is ticked (and says that it would take none), nor
                // for the first moment the list is looked at, nor for the first moment after
                // something came to be ticked where nothing was or a row was opened or closed,
                // nor while lines beside the devices are folded away that nobody opened: the
                // program takes none then, and the button looks so.
                Button {
                    objectName: "ownImportTake"
                    width: parent.width
                    visible: box.files.laidOut
                    primary: true
                    enabled: !box.files.busy && box.files.mayTake
                    text: box.files.hasOtherParts ? qsTr("Import %1").arg(box.files.tickedCount) : box.files.tickedCount === 1 ? Words.takeOneKvm : Words.takeKvms.arg(box.files.tickedCount)
                    onClicked: box.files.take()
                }
            }
            // (While a file is read it closes the box over that: the reading is waited for no
            // more, and nothing of it is laid out. Once a taking is over there is nothing to take
            // back, and it only closes; what came of the taking is told in the window's corner.
            // As wide as the wider of what it says: "Close" stands just where "Cancel" stood.)
            TextMetrics {
                id: cancelWords

                font.family: Application.font.family
                font.pixelSize: Theme.textSize
                font.weight: Font.Medium
                text: Words.cancel
            }
            TextMetrics {
                id: closeWords

                font.family: Application.font.family
                font.pixelSize: Theme.textSize
                font.weight: Font.Medium
                text: Words.close
            }
            Button {
                objectName: "ownImportCancel"
                anchors.right: own.left
                anchors.rightMargin: 8
                width: Math.ceil(Math.max(cancelWords.advanceWidth, closeWords.advanceWidth)) + 26
                text: box.files.took ? Words.close : Words.cancel
                onClicked: box.close()
            }
        }
    }
    Connections {
        // The file was read: what it was read with is needed no more, and is out of its line.
        // The list begins at its top, with nothing kept of the one before: not the place that one
        // was scrolled to, no room left after its end, and no row of the keyboard's. And the
        // keyboard is at the list, which its keys scroll, and at no button.
        function onFileWasRead() {
            passphrase.text = "";
            list.currentIndex = -1;
            list.bottomMargin = 0;
            list.positionViewAtBeginning();
            list.forceActiveFocus();
        }
        // The file a passphrase was typed for is not the chosen one any more: it is not another
        // file's, and is out of its line.
        function onFileIsAnother() {
            passphrase.text = "";
        }
        // A taking is over, and the box says nothing of it: where every one that was ticked is on
        // the wall it closes, and how many is told in this window's corner. And where the box was
        // closed in the middle of a taking, or closed over what came of one, that is told there:
        // as something to be looked at, and kept for when the window is seen again where it is
        // out of sight now.
        function onTaken(said: string, all: bool) {
            box.close();
            if (all)
                box.actions.tell(said, true);
            else
                box.actions.tellWhenSeen(said, false);
        }
        // The program closes the box: its window is out of sight.
        function onCloseAsked() {
            box.close();
        }

        target: box.files
    }
}
