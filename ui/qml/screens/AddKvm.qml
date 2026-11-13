import QtQuick
import Qkvm

// A KVM is added to this PC's own wall: where it is, and what it is signed in to with. The page's
// box for it, for one KVM at a time. It is asked who it is before the password goes anywhere, and
// what it is known by from then on is the certificate it showed.
Dialog {
    id: box

    required property AddKvmView adding
    required property ActionsView actions
    // Main supplies the wall/window context; standalone screen rigs need no new required value.
    property bool discoveryAllowed: true

    onDiscoveryAllowedChanged: {
        if (!discoveryAllowed)
            discoveryBox.close();
    }

    DiscoveryView {
        id: discovering

        objectName: "discoveryView"
        active: discoveryBox.visible && box.visible && box.discoveryAllowed && !box.adding.busy
        onAddressChosen: (found, family) => {
            address.text = found;
            box.chosen = family === "jetkvm" ? found : "";
            discoveryBox.close();
            password.takeFocus();
        }
    }
    DiscoverKvms {
        id: discoveryBox

        objectName: "discoverKvms"
        discovery: discovering
    }
    inTitle: Button {
        objectName: "discoverKvmOpen"
        enabled: box.discoveryAllowed && !box.adding.busy
        text: Words.discoverKvms
        onClicked: discoveryBox.open()
    }
    // How many were added when the box last looked: one more, and the next is typed.
    property int counted: 0
    // Whether a KVM was being asked when the box last looked, and the line the keyboard was in
    // last: it is there again when the asking is over and nothing was added (a click on a button
    // of the box takes the keyboard away).
    property bool asking: false
    property Field typedInLast: null
    // The address a search found a JetKVM at, which has no user name: while that address stands in
    // its line, the line for one is not shown, and none is sent.
    property string chosen: ""
    readonly property bool forAJetKvm: chosen !== "" && address.text === chosen
    // Something was typed that is not added yet: an update that would close the program waits for it.
    readonly property bool unsaved: opened && (address.text !== "" || user.text !== "" || password.text !== "" || group.text !== "")

    // What the window has room for, for the lines to type into, beside everything else in the box:
    // in a small window they are scrolled, and what came of the adding and the buttons stay in sight.
    readonly property real room: Math.max(60, (parent ? parent.height : 700) - 24 - 56 - 36 - (said.visible ? said.height + 14 : 0) - 14 - Theme.controlHeight)

    title: Words.addAKvm
    width: Math.min(520, parent ? parent.width - 24 : 520)
    // Not by a slip of the hand while a KVM is being asked: that is given up with the button
    // that says so.
    canClose: !adding.busy
    closesBeside: !adding.busy

    function submit() {
        // (What is sent is on show no longer, as on the page.)
        password.conceal();
        if (!adding.busy)
            adding.add(address.text, forAJetKvm ? "" : user.text, password.text, group.text);
    }
    function keepBoth() {
        password.conceal();
        if (!adding.busy)
            adding.addKeepingBoth(address.text, forAJetKvm ? "" : user.text, password.text, group.text);
    }

    // The line the keyboard has come to is in sight.
    function bringIntoSight(line) {
        if (!line.hasKeyboard)
            return;
        typedInLast = line;
        if (line.y < lines.contentY)
            lines.contentY = line.y;
        else if (line.y + line.height > lines.contentY + lines.height)
            lines.contentY = line.y + line.height - lines.height;
    }

    onAboutToShow: adding.reset()
    onOpened: address.takeFocus()
    // What was typed is not kept: not the password, and not for the next time the box is opened.
    // (Closed from outside over a KVM that is being asked, the wall having been left or the
    // window gone out of sight: the program is told, and gives that up.)
    onAboutToHide: discoveryBox.close()
    onClosed: {
        discoveryBox.close();
        address.text = "";
        user.text = "";
        password.text = "";
        group.text = "";
        chosen = "";
        lines.contentY = 0;
        adding.closed();
    }

    Connections {
        target: box.adding

        // It is on the wall: the next one is typed over what stood there, with the same sign-in.
        // (Only then: what was typed for one that was refused or given up stands.)
        function onChanged() {
            if (box.adding.count > box.counted) {
                address.text = "";
                address.takeFocus();
            } else if (box.asking && !box.adding.busy && box.typedInLast !== null) {
                box.typedInLast.takeFocus();
            }
            box.counted = box.adding.count;
            box.asking = box.adding.busy;
        }
        // The program closes the box: its window is out of sight.
        function onCloseAsked() {
            box.close();
        }
        // A KVM was added in the very turn in which the box was closed from outside: the box
        // says nothing any more, so it is told in the window's corner, where it stays until
        // somebody sees the window. With the certificate the box would have shown.
        function onAddedAsItClosed(said: string, certificate: string) {
            // (The second into its place first, and each as it is: a KVM may be called "%2".)
            box.actions.tellWhenSeen(certificate !== "" ? Words.oneThenAnother.replace("%2", () => Words.rememberedSha256.arg(certificate)).replace("%1", () => said) : said, true);
        }
    }

    Column {
        width: box.availableWidth
        spacing: 14

        Flickable {
            id: lines

            objectName: "kvmLines"
            width: parent.width
            height: Math.min(typed.height, box.room)
            contentHeight: typed.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: typed

                width: lines.width
                spacing: 14

                Field {
                    id: address

                    objectName: "kvmAddress"
                    width: parent.width
                    enabled: !box.adding.busy
                    label: Words.address
                    placeholder: "192.168.1.20"
                    onAccepted: password.takeFocus()
                    onHasKeyboardChanged: box.bringIntoSight(address)
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: Words.addressOfAKvmHint
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.WordWrap
                }
                Field {
                    id: user

                    objectName: "kvmUser"
                    width: parent.width
                    visible: !box.forAJetKvm
                    enabled: !box.adding.busy
                    label: Words.userName
                    placeholder: "admin"
                    onAccepted: password.takeFocus()
                    onHasKeyboardChanged: box.bringIntoSight(user)
                }
                Field {
                    id: password

                    objectName: "kvmPassword"
                    width: parent.width
                    enabled: !box.adding.busy
                    label: Words.password
                    secret: true
                    onAccepted: box.submit()
                    onHasKeyboardChanged: box.bringIntoSight(password)
                }
                Field {
                    id: group

                    objectName: "kvmGroup"
                    width: parent.width
                    enabled: !box.adding.busy
                    label: Words.group
                    placeholder: Words.none
                    onAccepted: box.submit()
                    onHasKeyboardChanged: box.bringIntoSight(group)
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: Words.groupHint
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.WordWrap
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: Words.itsCertificateIsRemembered
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.WordWrap
                }
            }
        }
        // What came of the adding: why not, or that it is on the wall.
        Column {
            id: said

            width: parent.width
            visible: box.adding.busy || box.adding.problem !== "" || box.adding.added !== ""

            Text {
                id: problem

                textFormat: Text.PlainText
                objectName: "kvmProblem"
                width: parent.width
                visible: text !== ""
                text: box.adding.problem
                color: Theme.bad
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
            // It says by its number that it is a KVM of the wall: the person may keep both, as on the page.
            Button {
                objectName: "kvmKeepBoth"
                visible: box.adding.mayKeepBoth && !box.adding.busy
                text: Words.keepBoth
                onClicked: box.keepBoth()
            }
            Text {
                id: added

                textFormat: Text.PlainText
                objectName: "kvmAdded"
                width: parent.width
                visible: text !== "" && box.adding.problem === ""
                text: box.adding.busy ? Words.checking : box.adding.added
                color: box.adding.busy ? Theme.muted : Theme.ok
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
            // What it is known by from now on, to be compared with what the KVM says of itself.
            Text {
                textFormat: Text.PlainText
                objectName: "kvmCertificate"
                width: parent.width
                topPadding: 4
                visible: added.visible && !box.adding.busy && box.adding.certificate !== ""
                text: Words.rememberedSha256.arg(box.adding.certificate)
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WrapAnywhere
            }
        }
        Item {
            width: parent.width
            height: Theme.controlHeight

            Row {
                anchors.right: parent.right
                spacing: 8

                Button {
                    objectName: "kvmClose"
                    // While a KVM is being asked it gives that up, and the box stays.
                    text: box.adding.count > 0 && !box.adding.busy ? Words.done : Words.cancel
                    onClicked: box.adding.busy ? box.adding.giveUp() : box.close()
                }
                Button {
                    objectName: "kvmAdd"
                    primary: true
                    enabled: !box.adding.busy
                    text: Words.checkAndAdd
                    onClicked: box.submit()
                }
            }
        }
    }
}
