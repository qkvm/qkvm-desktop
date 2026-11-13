pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// The settings' section of this PC's KVMs, the page's "KVMs": every KVM of this PC's wall as a row,
// what it is called and under that, in the colour of a problem, where it is the same device as
// another of the wall or no password is kept for it; where it is and under that what kind of KVM; how
// it is spoken to; and at the row's end "Edit" (the box that changes a KVM) and "Remove" (the same
// box, at its question first), its words in red as the page's are. "Add KVMs" in its first line
// opens the box that adds one. The list holds still while it is shown (ui/own_kvms_view.h): a row
// that was taken off says so, and its buttons are there and take no click; and every row is of one
// height, whatever it says (Listed.qml).
//
// Where the keyboard was at a row's button and that row's KVM is taken off (by its question, or
// elsewhere), the keyboard does not stay on nothing: it goes to the button of the next row that
// takes a press, and where none does, to the section's entry in the box's list (`keyboardToTheList`).
// So it does where the row itself went, button and all (the list was shown anew, and the wall was
// another by then): to the section's entry. (A row with the keyboard in it is not let go for being
// far out of sight: Listed.qml.)
Section {
    id: section

    required property OwnKvmsView kvms
    // It is shown: its box is open on it, in a window that is in sight. While it is, the list holds
    // still; each box that shows it says so, and that it does no longer, once.
    property bool shown: false
    // (Whether this one has said so: once each way.)
    property bool says: false
    // The button of a row the keyboard was at last, and the KVM of that row.
    property Item keyboardWasAt: null
    property string keyboardWasWith

    signal addAsked
    signal editAsked(string machine)
    signal removeAsked(string machine)
    signal keyboardToTheList

    function sayWhetherShown() {
        if (shown === says)
            return;
        says = shown;
        if (shown)
            kvms.begin();
        else
            kvms.end();
    }
    // The row an item of a row stands in, in this list; null where it stands in none.
    function rowOf(item: Item): ListRow {
        for (let up = item; up !== null; up = up.parent) {
            const row = up as ListRow;
            if (row !== null)
                return row.of === list ? row : null;
        }
        return null;
    }
    // Whether the keyboard rests on what the section stands in, and not on any control of it.
    function restsAbove(item: Item): bool {
        for (let up = section; up !== null; up = up.parent) {
            if (up === item)
                return true;
        }
        return false;
    }
    // The KVM of a row of this list, by the wall's name for it; empty: none.
    function kvmOf(row): string {
        return row !== null ? row.key ?? "" : "";
    }
    function keyboardMoved() {
        const now = section.Window.activeFocusItem;
        const row = now !== null ? rowOf(now) : null;
        if (row !== null) {
            keyboardWasAt = now;
            keyboardWasWith = kvmOf(row);
            return;
        }
        // Come to rest on what the section stands in: looked at a turn of the loop later, once
        // what is being let go or made anew is (a row that goes takes the keyboard with it on its
        // way out).
        if (now !== null && restsAbove(now) && keyboardWasWith !== "")
            Qt.callLater(section.keyboardRests);
    }
    function keyboardRests() {
        const now = section.Window.activeFocusItem;
        const was = keyboardWasAt;
        const kvm = keyboardWasWith;
        if (now === null || !restsAbove(now) || kvm === "")
            return;
        // The row of that KVM as it is made now, and whether the button stands in it still.
        const row = list.rowWhere(one => section.kvmOf(one) === kvm);
        if (was === null || row === null || rowOf(was) !== row) {
            // It went, row and button: the list was shown anew. (Its KVM may be there, in a row
            // that was made anew; the keyboard was at none of that one's.)
            keyboardWasAt = null;
            keyboardWasWith = "";
            keyboardToTheList();
            return;
        }
        // (There still, and taking a press: the keyboard was put above it for a moment.)
        if (was.enabled)
            return;
        keyboardWasAt = null;
        keyboardWasWith = "";
        // Its KVM was taken off: the next row's button that takes a press. (The rows after it that
        // are made: those far out of sight are not, and are not looked for.)
        for (let next = list.rowAt(row.index + 1); next !== null; next = list.rowAt(next.index + 1)) {
            const button = next.firstThatTakesAPress();
            if (button !== null) {
                button.forceActiveFocus(Qt.OtherFocusReason);
                return;
            }
        }
        keyboardToTheList();
    }

    objectName: "settingsKvms"
    name: Words.kvms
    about: Words.kvmsAbout
    onShownChanged: sayWhetherShown()
    Component.onCompleted: sayWhetherShown()
    Component.onDestruction: {
        if (says)
            kvms.end();
    }

    Connections {
        function onActiveFocusItemChanged() {
            section.keyboardMoved();
        }

        target: section.Window
    }

    does: Button {
        objectName: "settingsAddKvms"
        primary: true
        symbol: "plus"
        text: Words.addKvms
        onClicked: section.addAsked()
    }

    Listed {
        id: list

        objectName: "kvmList"
        columns: [Words.name, Words.address, Words.connection]
        // What it is called, and under it its twins and whether no password is kept; where it is,
        // and under it what kind of KVM; how it is spoken to, which may take two lines.
        lines: [
            {
                "text": 1,
                "under": [1, 1]
            },
            {
                "text": 1,
                "under": [1]
            },
            {
                "text": 2
            }
        ]
        doesWidth: Math.ceil(editWidth.advanceWidth + removeWidth.advanceWidth) + 2 * 24 + 8
        nothing: Words.noKvmsYetAddTheFirst
        model: section.kvms

        delegate: ListRow {
            id: row

            required property string key
            required property string name
            required property string host
            required property string way
            required property string twins
            required property bool noPassword
            required property bool gone
            required property string family
            // What is said under its name, each in its tone: the others that are the same device, and
            // that no password is kept. That is a problem but of a JetKVM, which runs without one as it
            // comes out of its box: said so, and not as a problem.
            readonly property var saidUnder: [twins !== "" ? {
                    "said": Words.sameDeviceAs.arg(twins),
                    "tone": "bad"
                } : null, !noPassword ? null : family === "jetkvm" ? {
                    "said": Words.noPasswordFineWithout,
                    "tone": ""
                } : {
                    "said": Words.noPasswordSaved,
                    "tone": "bad"
                }].filter(one => one !== null)

            objectName: "kvmRow"
            of: list
            cells: [
                {
                    "text": name,
                    "under": gone ? [Words.takenOffThisWall] : saidUnder.map(one => one.said),
                    "tone": gone ? "" : saidUnder.map(one => one.tone)
                },
                {
                    "text": host,
                    "under": [family === "jetkvm" ? Words.jetKvmKind : family === "redfish" ? Words.serverControllerKind : Words.kvmdKind]
                },
                {
                    "text": way === "certificate" ? Words.knownByItsCertificate : way === "windows" ? Words.knownByWindowsWord : Words.plainHttpInTheClear
                }
            ]

            Button {
                objectName: "kvmEdit"
                enabled: !row.gone
                text: Words.edit
                Accessible.name: Words.editNamed.arg(row.name)
                onClicked: section.editAsked(row.key)
            }
            Button {
                objectName: "kvmRemove"
                enabled: !row.gone
                risky: true
                text: Words.remove
                Accessible.name: Words.removeOne.arg(row.name)
                onClicked: section.removeAsked(row.key)
            }
        }
    }

    // How wide the two buttons of a row are: the column they stand in.
    TextMetrics {
        id: editWidth

        font.family: Application.font.family
        font.pixelSize: Theme.textSize
        font.weight: Font.Medium
        text: Words.edit
    }
    TextMetrics {
        id: removeWidth

        font.family: Application.font.family
        font.pixelSize: Theme.textSize
        font.weight: Font.Medium
        text: Words.remove
    }
}
