pragma ComponentBehavior: Bound

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
            // (What a search finds is a KVM or a server.)
            box.what = "auto";
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
    // What is added, as the page's "What to add" chooses it (app.js addDevicesForm): "auto", KVMs and servers, told
    // apart by what answers; "vnc", a VNC server, which speaks no HTTP and is asked nothing of the kind (the web's kind
    // "vnc"); "proxmox", virtual machines on Proxmox VE, signed in to with an API token (its ID as the user name, its
    // secret as the password), whose guests are listed to tick. One whose address begins with vnc:// or vncs:// is a
    // VNC server where KVMs and servers are chosen too; where Proxmox's guests are, what is typed is the address of a
    // Proxmox, whose guests are asked for (as the page asks them), and is refused as none.
    property string what: "auto"
    readonly property bool forAVncServer: what === "vnc" || (what === "auto" && /^\s*vncs?:\/\//i.test(address.text))
    readonly property bool forProxmox: what === "proxmox"
    // The ticked guests of the list, by number.
    property var ticked: ({})
    // Something was typed that is not added yet: an update that would close the program waits for it.
    readonly property bool unsaved: opened && (address.text !== "" || user.text !== "" || password.text !== "" || group.text !== "")

    // A Proxmox's guests are listed in a list of one height of its own, from the moment the box is for a Proxmox on,
    // whatever it lists: only that list scrolls, and the box holds still, its buttons where they were (review c9b-1,
    // M4). Eight lines of it, and fewer in a window too small for them, two at the least.
    readonly property real guestsHeight: Math.max(48, Math.min(192, (parent ? parent.height : 700) - 400))
    // What a run of adds came to, of a Proxmox's guests, likewise in a place of one height of its own that alone
    // scrolls, the refused and the added lines together (review c9b-2, M2): four lines, two in the smallest window.
    readonly property real outcomeHeight: Math.max(2 * saying.height, Math.min(4 * saying.height, (parent ? parent.height : 700) - 500))

    // What is said under the lines of a Proxmox's guests: what the list is (three lines), the list, the button row and
    // what a run came to.
    readonly property real proxmoxSaid: 3 * saying.height + guestsHeight + Theme.controlHeight + outcomeHeight
    // The lines to type into and what is said under them take one height together, whatever is chosen: the box keeps
    // one size, and its controls their places, as the choice changes and as what came of an adding is said (the
    // boxes-hold-still rule; review c9b-4, L7). As much as a Proxmox's guests need, as far as the window has room; of
    // KVMs and VNC servers, the lines take what they need of it (four lines of what came of it kept at the least), and
    // what came of it the rest. In a small window the lines are scrolled, and what came of it and the buttons stay in
    // sight.
    readonly property real together: Math.max(60 + 14 + proxmoxSaid, Math.min((parent ? parent.height : 700) - 24 - 56 - 36 - 14 - Theme.controlHeight, typed.height + 14 + proxmoxSaid))
    readonly property real linesHeight: forProxmox ? together - 14 - proxmoxSaid : Math.min(typed.height, together - 14 - 4 * saying.height)

    title: Words.addAKvm
    width: Math.min(520, parent ? parent.width - 24 : 520)
    // Not by a slip of the hand while a KVM is being asked: that is given up with the button
    // that says so.
    canClose: !adding.busy
    closesBeside: !adding.busy

    // The address as it goes: where VNC servers are chosen, one typed without vnc:// or vncs:// is a VNC server's
    // all the same (vnc://, as the web's vnc_url takes it); and a VNC server is sent no user name. One that begins
    // with vnc or vncs and a colon or a slash goes as it was typed: with its scheme written amiss ("vncs:/box"), the
    // program refuses it, and it is not made "vnc://vncs:/box", a server at a host called "vncs" (outside review
    // c9a-3, L1).
    function typedAddress() {
        return what === "vnc" && !/^\s*vncs?[:\/]/i.test(address.text) ? "vnc://" + address.text.trim() : address.text;
    }
    function typedUser() {
        return forAJetKvm || forAVncServer ? "" : user.text;
    }
    function submit() {
        // (What is sent is on show no longer, as on the page.)
        password.conceal();
        if (adding.busy)
            return;
        if (forProxmox) {
            ticked = {};
            adding.listGuests(address.text, user.text, password.text, group.text);
            return;
        }
        adding.add(typedAddress(), typedUser(), password.text, group.text);
    }
    function addTicked() {
        password.conceal();
        const numbers = Object.keys(ticked).filter(vmid => ticked[vmid]).map(vmid => Number(vmid));
        if (!adding.busy && numbers.length > 0)
            adding.addGuests(address.text, user.text, password.text, group.text, numbers);
    }
    function keepBoth() {
        password.conceal();
        if (!adding.busy)
            adding.addKeepingBoth(typedAddress(), typedUser(), password.text, group.text);
    }
    // Its plain address sends everybody on to HTTPS: added at the HTTPS address it named, as on the page, once the person
    // says so (never by itself). The address line shows what is added.
    function useInstead() {
        if (adding.busy || adding.useInstead === "")
            return;
        address.text = adding.useInstead;
        submit();
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
        what = "auto";
        ticked = {};
        lines.contentY = 0;
        outcome.contentY = 0;
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
            // (A run that begins is said from its first line on, follows its newest line while it goes, and is said
            // from its first line again, the refused ones, when it is over: review c9b-3, L3.)
            if (box.adding.busy !== box.asking)
                outcome.contentY = 0;
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
            height: box.linesHeight
            contentHeight: typed.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: typed

                width: lines.width
                spacing: 14

                // What is added (the page's "What to add"): it stands at the top, and what it changes below it
                // changes in place (the address's hint stands where its tallest would, the user name's line stays
                // where it is), so nothing moves under the hand.
                Column {
                    width: parent.width
                    spacing: 6

                    Text {
                        textFormat: Text.PlainText
                        text: Words.whatToAdd
                        color: Theme.text
                        font.pixelSize: Theme.textSize
                        font.weight: Font.Medium
                    }
                    Choice {
                        objectName: "kvmWhat"
                        width: parent.width
                        enabled: !box.adding.busy
                        hint: Words.whatToAdd
                        choices: [
                            {
                                "value": "auto",
                                "label": Words.kvmsAndServers
                            },
                            {
                                "value": "vnc",
                                "label": Words.vncServers
                            },
                            {
                                "value": "proxmox",
                                "label": Words.proxmoxGuests
                            }
                        ]
                        value: box.what
                        onPicked: chosenWhat => {
                            box.what = chosenWhat;
                            box.ticked = {};
                        }
                    }
                }
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
                // The address's hint, in a place as high as the tallest of the hints it may show (one for each
                // choice), whichever stands: what is under it stays where it is when the choice changes it, however
                // long a hint's words are in a translation or a font (the second reading of c9a, difference 5). A
                // hint that comes for another choice is put in `hints`, and measured with the rest.
                Item {
                    id: hintPlace

                    readonly property var hints: [Words.addressOfAKvmHint, Words.addressOfAVncServerHint, Words.addressOfAProxmoxHint]
                    property real tallest: 0

                    function measure() {
                        let most = 0;
                        for (let at = 0; at < measured.count; ++at) {
                            const one = measured.itemAt(at);
                            if (one !== null)
                                most = Math.max(most, one.implicitHeight);
                        }
                        tallest = most;
                    }

                    objectName: "kvmAddressHintPlace"
                    width: parent.width
                    height: Math.max(tallest, hint.implicitHeight)

                    Text {
                        id: hint

                        objectName: "kvmAddressHint"
                        textFormat: Text.PlainText
                        width: parent.width
                        text: box.forProxmox ? Words.addressOfAProxmoxHint : box.forAVncServer ? Words.addressOfAVncServerHint : Words.addressOfAKvmHint
                        color: Theme.muted
                        font.pixelSize: Theme.smallSize
                        wrapMode: Text.WordWrap
                    }
                    // (Never seen: each hint as high as it is at this width.)
                    Repeater {
                        id: measured

                        model: hintPlace.hints
                        onItemAdded: hintPlace.measure()
                        onItemRemoved: hintPlace.measure()

                        Text {
                            required property string modelData

                            objectName: "kvmAddressHintMeasure"
                            visible: false
                            textFormat: Text.PlainText
                            width: hintPlace.width
                            text: modelData
                            font.pixelSize: Theme.smallSize
                            wrapMode: Text.WordWrap
                            onImplicitHeightChanged: hintPlace.measure()
                        }
                    }
                }
                Field {
                    id: user

                    objectName: "kvmUser"
                    width: parent.width
                    visible: !box.forAJetKvm
                    // (A VNC server has none: the line stays, and is not typed into, as on the page. A Proxmox's is its
                    // API token's ID.)
                    enabled: !box.adding.busy && !box.forAVncServer
                    label: box.forProxmox ? Words.apiTokenId : Words.userName
                    placeholder: box.forProxmox ? "user@pve!wall" : "admin"
                    onAccepted: password.takeFocus()
                    onHasKeyboardChanged: box.bringIntoSight(user)
                }
                Field {
                    id: password

                    objectName: "kvmPassword"
                    width: parent.width
                    enabled: !box.adding.busy
                    label: box.forProxmox ? Words.apiTokenSecret : Words.password
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
        // What came of the adding: why not, or that it is on the wall. In its place of one height whatever is chosen
        // and said (box.together).
        Column {
            id: said

            objectName: "kvmSaid"
            width: parent.width
            height: box.together - 14 - box.linesHeight

            // A Proxmox's guests, to tick (the page's picklist): one that is on the wall already, or a new virtual
            // machine under a card's number, may not be ticked, and says why. Over them what the list is: what comes
            // there before anything was listed, how many there are and the containers left out, or, of a list that
            // has none, why that may be (review c9b-1, L3). Three lines at the most, in one place.
            Text {
                textFormat: Text.PlainText
                objectName: "kvmGuestsSaid"
                width: parent.width
                height: 3 * saying.height
                visible: box.forProxmox
                text: (!box.adding.listed ? Words.guestsListedHere : box.adding.guests.length > 0 ? Words.guestsToTick.arg(box.adding.guests.length) : Words.noGuestSeen) + (box.adding.listed && box.adding.containers > 0 ? " " + Words.containersNotListed.arg(box.adding.containers) : "")
                color: box.adding.listed ? Theme.text : Theme.muted
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight

                FontMetrics {
                    id: saying

                    font.pixelSize: Theme.textSize
                }
            }
            ListView {
                id: guestList

                objectName: "kvmGuests"
                width: parent.width
                height: box.guestsHeight
                visible: box.forProxmox
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                spacing: 4
                model: box.forProxmox ? box.adding.guests : []

                delegate: Check {
                    required property var modelData

                    objectName: "kvmGuest" + modelData.vmid
                    width: guestList.width
                    enabled: !box.adding.busy && modelData.taken === ""
                    text: modelData.vmid + " · " + modelData.name + " · " + (modelData.taken !== "" ? modelData.taken : modelData.node + (modelData.status !== "" ? " · " + modelData.status : ""))
                    checked: box.ticked[modelData.vmid] === true
                    onToggled: {
                        const now = Object.assign({}, box.ticked);
                        now[modelData.vmid] = checked;
                        box.ticked = now;
                    }
                }
            }
            // (That there are more guests below than are seen.)
            ScrollMark {
                objectName: "kvmGuestsMark"
                flickable: guestList
            }
            // The ticked ones are added from here; beside it, in a place kept for it, that a Proxmox is being asked.
            Item {
                width: parent.width
                height: Theme.controlHeight
                visible: box.forProxmox

                Button {
                    id: addTicked

                    objectName: "kvmAddTicked"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: !box.adding.busy && box.adding.guests.length > 0
                    text: Words.addTheTicked
                    onClicked: box.addTicked()
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "kvmChecking"
                    anchors.left: addTicked.right
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: box.adding.busy ? Words.checking : ""
                    color: Theme.muted
                    font.pixelSize: Theme.textSize
                    elide: Text.ElideRight
                }
            }
            // Why not, and what was added: in its place of one height, which alone scrolls. Of a Proxmox, the refused
            // lines first and every added line after them, none hiding the other (review c9b-2, M2); of a KVM, the
            // whole of what is said under its lines.
            Flickable {
                id: outcome

                objectName: "kvmOutcome"
                width: parent.width
                height: box.forProxmox ? box.outcomeHeight : said.height
                contentHeight: outcomeLines.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // (While a run goes, its newest line is in sight.)
                onContentHeightChanged: {
                    if (box.forProxmox && box.adding.busy)
                        contentY = Math.max(0, contentHeight - height);
                }

                Column {
                    id: outcomeLines

                    width: outcome.width

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
                    // Its plain address sends everybody on to HTTPS: that address is offered, as on the page (web 0.3.2).
                    Button {
                        objectName: "kvmUseInstead"
                        visible: box.adding.useInstead !== "" && box.adding.problem !== "" && !box.adding.busy
                        text: Words.useInstead.arg(box.adding.useInstead)
                        onClicked: box.useInstead()
                    }
                    Text {
                        id: added

                        textFormat: Text.PlainText
                        objectName: "kvmAdded"
                        width: parent.width
                        visible: text !== "" && (box.forProxmox || box.adding.problem === "")
                        text: box.adding.busy && !box.forProxmox ? Words.checking : box.adding.added
                        color: box.adding.busy && !box.forProxmox ? Theme.muted : Theme.ok
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
            }
            // (That there is more of what it says than is seen.)
            ScrollMark {
                objectName: "kvmOutcomeMark"
                flickable: outcome
            }
        }
        Item {
            width: parent.width
            height: Theme.controlHeight

            Row {
                anchors.right: parent.right
                spacing: 8

                Button {
                    id: closing

                    objectName: "kvmClose"
                    // While a KVM is being asked it gives that up, and the box stays. As wide with either word: it
                    // keeps its size and place when one turns into the other (review c9b-2, M2).
                    text: box.adding.count > 0 && !box.adding.busy ? Words.done : Words.cancel
                    width: Math.ceil(Math.max(cancelWidth.advanceWidth, doneWidth.advanceWidth)) + leftPadding + rightPadding
                    onClicked: box.adding.busy ? box.adding.giveUp() : box.close()

                    TextMetrics {
                        id: cancelWidth

                        font: closing.font
                        text: Words.cancel
                    }
                    TextMetrics {
                        id: doneWidth

                        font: closing.font
                        text: Words.done
                    }
                }
                Button {
                    id: adds

                    objectName: "kvmAdd"
                    primary: true
                    enabled: !box.adding.busy
                    text: box.forProxmox ? Words.listItsGuests : Words.checkAndAdd
                    // As wide with either word: it, and Cancel beside it, keep their places when the choice changes
                    // one into the other (review c9b-4, L7).
                    width: Math.ceil(Math.max(checkWidth.advanceWidth, listWidth.advanceWidth)) + leftPadding + rightPadding
                    onClicked: box.submit()

                    TextMetrics {
                        id: checkWidth

                        font: adds.font
                        text: Words.checkAndAdd
                    }
                    TextMetrics {
                        id: listWidth

                        font: adds.font
                        text: Words.listItsGuests
                    }
                }
            }
        }
    }
}
