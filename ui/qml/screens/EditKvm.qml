pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// A KVM of this PC's own wall is changed, or taken off the wall: the page's box for it. In the
// place of its lines it asks before the KVM is taken off, and shows the certificate the KVM is
// known by beside the one it shows right now, or what it is known by beside what it would be
// known by at an address that was typed for it, to be said yes to with a click and with nothing
// else. What it asks is the program's to do (ui/own_wall.h): this shows what it is given, and
// keeps nothing of what was typed once it is closed.
Dialog {
    id: box

    required property EditKvmView editing
    required property ActionsView actions

    // What stands in the place of the lines: the question before the KVM is taken off the wall;
    // and the two certificates, once they were read, and from the first where the box was opened
    // for them alone. Two certificates stand there too where a saving was refused over the one
    // that is shown at the address that was typed: the lines keep what was typed meanwhile.
    property bool removing: false
    readonly property bool reviewing: editing.reviewed || editing.forReview
    readonly property bool differing: editing.differs
    readonly property bool comparing: reviewing || differing
    readonly property bool typing: !removing && !comparing
    // A line differs from what stood in it when the box was opened (a password typed at all): an update that would close
    // the program waits for it.
    readonly property bool unsaved: opened && typing && (name.text !== editing.name || address.text !== editing.address || user.text !== editing.user || password.text !== "" || group.text !== editing.group || noSignal.text !== (editing.noSignalMinutes > 0 ? String(editing.noSignalMinutes) : "") || unchanged.text !== (editing.unchangedMinutes > 0 ? String(editing.unchangedMinutes) : ""))
    // What is shown is the certificate another KVM of the wall is known by: no yes is taken to
    // it. Otherwise one is offered wherever the two that stand there are not the same.
    readonly property bool isAnothers: editing.presentedIsCalled !== ""
    readonly property bool offersAYes: ((editing.reviewed && !editing.same) || differing) && !isAnothers
    // What is said under the two, by what stands there: whose the second is; what a yes means
    // where it is Windows' word that the KVM would be known by, or where it is known by none
    // yet; and otherwise the page's warning.
    readonly property string saidOfTheTwo: isAnothers ? Words.showsTheCertificateOf.arg(editing.presentedIsCalled) : editing.onWindowsWord ? (editing.remembered !== "" ? Words.yesMeansWindowsWordNotItsCertificate : Words.yesMeansWindowsWord) : differing && editing.remembered === "" ? (editing.https ? Words.knownByWindowsWordSoNoneToCompareWith : Words.noCertificateToCompareWith) : Words.aNewCertificateOrAnotherDevice
    // And what is said in its place where what is shown is another than a yes was given to.
    // Where that yes was to Windows' word, no certificate stood there that this one could be
    // "another than": it is said what Windows makes of what is shown now.
    readonly property string saidOfAnother: editing.yesWasOnWindowsWord ? (editing.onWindowsWord ? Words.afterWindowsWordAnotherTrusted + " " + saidOfTheTwo : Words.afterWindowsWordNotTrusted) : editing.onWindowsWord ? Words.shownAnotherThanSaidYesTo + " " + saidOfTheTwo : Words.shownAnotherThanSaidYesTo
    // The yes to what is shown at a new address is a saving, with what stands in the lines.
    readonly property bool trusting: editing.asked === "trusting" || (differing && editing.asked === "saving")
    // What was asked last was asked from what is shown now: why it was not done is said there,
    // and not under something else. (A yes that was refused because the KVM is another by now
    // has the box back at its lines, filled anew: said there.)
    readonly property bool askedHere: comparing ? editing.asked === "looking" || trusting : removing ? editing.asked === "removing" : editing.asked === "saving" || editing.asked === "looking" || editing.asked === "trusting"
    // Whether something was being asked when the box last looked, and the line the keyboard was in
    // last: it is there again when the asking is over (a click on a button of the box takes the
    // keyboard away).
    property bool asking: false
    property Field typedInLast: null
    // Its KVM was taken off the wall elsewhere while it is open (and what it was asking then, if
    // anything, is over): there is nothing of it to change, to take off or to say yes to any more.
    // The box stays where it is, and as large, whatever it shows. At its question before a KVM is
    // taken off (from its lines, or a list's "Remove") that is said where the question's words
    // stood; otherwise where it says what came of something, in room that what stands above gives
    // up for it (that is cut shorter, and scrolls). Its buttons stand where they stood; those that
    // would do something take no click, and the way out closes it and has the keyboard. Nothing
    // comes to stand where something stood under the pointer.
    readonly property bool gone: editing.gone && !editing.busy
    // That it went is shown: from the moment the height the box held then was taken (holdTheHeight),
    // which is what the box is laid out by from then on.
    property bool goneShown: false
    // (Said where the question's words stood.)
    readonly property bool goneAtTheQuestion: goneShown && removing && !comparing
    // How high what it holds is, and was when its KVM went: it stays so (but where the window has
    // less room).
    property real heldHeight: 0

    // What the window has room for, beside everything else in the box: in a small window what
    // stands above is scrolled, and what came of the asking and the buttons stay in sight.
    readonly property real room: Math.max(60, (parent ? parent.height : 700) - 24 - 56 - 36 - (said.visible ? said.height + 14 : 0) - 14 - Theme.controlHeight)

    // A button that says "Checking…" while what it asked for is under way, as wide as it is with
    // either: the buttons beside it stay where they are.
    component Asking: Button {
        id: asks

        property string says
        property bool underWay: false

        text: underWay ? Words.checking : says
        width: Math.ceil(Math.max(saysWidth.advanceWidth, checkingWidth.advanceWidth)) + leftPadding + rightPadding

        TextMetrics {
            id: saysWidth

            font: asks.font
            text: asks.says
        }
        TextMetrics {
            id: checkingWidth

            font: asks.font
            text: Words.checking
        }
    }

    // A certificate's SHA-256 under what it is, in letters that are all of one width; or, in its
    // place, that there is none, or what stands for one. As high with the one as with the other:
    // two of them stand side by side, to be compared line by line. One that is another than a
    // moment ago is marked (`marked`): what stands over it begins with that word, in the
    // warning's colour, and its frame has that colour. And what stands in the place of one is
    // in that colour where it says what is given up (`grave`).
    component Fingerprint: Column {
        id: fingerprint

        property string label
        property string written
        property string instead
        property bool marked: false
        property bool grave: false

        // In four lines of eight, as it is easiest to compare by eye: whole, never cut.
        readonly property string inLines: {
            const pairs = written.split(":");
            const lines = [];
            for (let at = 0; at < pairs.length; at += 8)
                lines.push(pairs.slice(at, at + 8).join(":"));
            return lines.join("\n");
        }

        spacing: 5

        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: fingerprint.marked ? Words.changedThen.arg(fingerprint.label) : fingerprint.label
            color: fingerprint.marked ? Theme.warn : Theme.muted
            font.pixelSize: Theme.textSize
            font.weight: fingerprint.marked ? Font.DemiBold : Font.Normal
            // (An address in it has no blank to be broken at.)
            wrapMode: Text.Wrap
        }
        Rectangle {
            width: parent.width
            height: Math.max(letters.implicitHeight, fourLines.implicitHeight) + 16
            radius: Theme.radius
            color: Theme.inset
            border.width: fingerprint.marked ? 2 : 1
            border.color: fingerprint.marked ? Theme.warn : Theme.lineStrong

            Text {
                id: letters

                textFormat: Text.PlainText
                anchors.centerIn: parent
                // (What stands in the place of one is a sentence, and is broken into lines; an
                // address in it has no blank to be broken at.)
                width: parent.width - 16
                horizontalAlignment: Text.AlignHCenter
                wrapMode: fingerprint.written !== "" ? Text.NoWrap : Text.Wrap
                text: fingerprint.written !== "" ? fingerprint.inLines : fingerprint.instead
                color: fingerprint.written !== "" ? Theme.text : fingerprint.grave ? Theme.warn : Theme.muted
                font.family: fingerprint.written !== "" ? "Consolas" : Application.font.family
                font.pixelSize: Theme.textSize
                lineHeight: 1.25
                Accessible.name: fingerprint.label + " " + (fingerprint.written !== "" ? fingerprint.written : fingerprint.instead)
            }
            // (Never seen: how high four such lines are.)
            Text {
                id: fourLines

                textFormat: Text.PlainText
                visible: false
                text: "0\n0\n0\n0"
                font.family: "Consolas"
                font.pixelSize: Theme.textSize
                lineHeight: 1.25
            }
        }
    }

    // (What the KVM is called stands in each of them as a sentence calls it, set off: a name
    // is free text, and "Verified" before "certificate" is not to read as the program's word.)
    title: comparing ? Words.certificateOf.arg(editing.calledInASentence) : removing ? Words.removeNamed.arg(editing.calledInASentence) : Words.editNamed.arg(editing.calledInASentence)
    width: Math.min(520, parent ? parent.width - 24 : 520)
    // Not by a slip of the hand while something is asked: that is given up with the button
    // that says so.
    canClose: !editing.busy
    closesBeside: !editing.busy

    function submit() {
        // (What is sent is on show no longer, as on the page.)
        password.conceal();
        if (!editing.busy && !gone) {
            editing.typeWatches(noSignal.text, unchanged.text);
            editing.save(name.text, address.text, user.text, password.text, group.text);
        }
    }
    function keepBoth() {
        password.conceal();
        if (!editing.busy && !gone) {
            editing.typeWatches(noSignal.text, unchanged.text);
            editing.saveKeepingBoth(name.text, address.text, user.text, password.text, group.text);
        }
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

    // The KVM as it is on the wall stands in the lines, and no password. (A box that was opened for
    // the question before the KVM is taken off, a list's "Remove", shows that question.)
    function fill() {
        name.text = editing.name;
        address.text = editing.address;
        user.text = editing.user;
        password.text = "";
        group.text = editing.group;
        noSignal.text = editing.noSignalMinutes > 0 ? String(editing.noSignalMinutes) : "";
        unchanged.text = editing.unchangedMinutes > 0 ? String(editing.unchangedMinutes) : "";
        removing = editing.removesOnly;
        typedInLast = null;
        asking = editing.busy;
        lines.contentY = 0;
    }

    // The keyboard is where the next thing is typed; before the KVM is taken off, at the no. A
    // certificate is not said yes to with a key that was meant for something else: while the two
    // are shown, the keyboard is at neither button. Where the KVM is gone, at the way out.
    function settle() {
        if (!opened)
            return;
        if (gone)
            (comparing ? reviewClose : removing ? removeNo : editCancel).forceActiveFocus();
        else if (comparing)
            certificates.forceActiveFocus();
        else if (removing)
            removeNo.forceActiveFocus();
        else if (typedInLast !== null)
            typedInLast.takeFocus();
        else
            name.takeFocus();
    }

    // The height it holds once its KVM went: what it holds as it is laid out at this moment, before
    // that is shown. (Laid out now, inner first: it may not have been drawn yet as it is now, as
    // where the KVM goes the moment the box is opened, and what it last was laid out by would be the
    // height of what it showed before.)
    function holdTheHeight() {
        both.forceLayout();
        certificates.forceLayout();
        typed.forceLayout();
        pages.forceLayout();
        said.forceLayout();
        holds.forceLayout();
        heldHeight = holds.height;
    }

    onOpened: settle()
    onGoneChanged: {
        if (gone)
            holdTheHeight();
        goneShown = gone;
        settle();
    }
    onComparingChanged: {
        lines.contentY = 0;
        settle();
    }
    onRemovingChanged: {
        lines.contentY = 0;
        settle();
    }
    // What was typed is not kept: not the password, and not for the next time the box is opened.
    // (Closed from outside over something that is being asked, the wall having been left: the
    // program is told, and gives that up.)
    onClosed: {
        name.text = "";
        address.text = "";
        user.text = "";
        password.text = "";
        group.text = "";
        noSignal.text = "";
        unchanged.text = "";
        removing = false;
        typedInLast = null;
        asking = false;
        lines.contentY = 0;
        editing.closed();
    }

    // The yes to a certificate, in the line of the box's title: where no other button of the box
    // stands in any of its states. The two certificates come when whatever answers at the KVM's
    // address has answered, and a click that was on its way to the button that stood under the
    // pointer a moment before is no yes. Nor is a click in the first moment after they have come
    // or have been looked at again (the program takes none then, and the button looks so), and
    // the keyboard does not come here at all: not with Tab, and no key presses it. It is not
    // there where what is shown is another KVM's of the wall: there is no yes to that.
    inTitle: Asking {
        objectName: "reviewTrust"
        visible: box.offersAYes
        enabled: !box.editing.busy && box.editing.mayTrust && !box.gone
        focusPolicy: Qt.NoFocus
        // (Where Windows vouches there, no certificate is shown to say yes to: its word is. And
        // where that address is plain HTTP, nothing is shown: the yes is to speaking to it so.)
        says: box.editing.plainHttpThere ? Words.usePlainHttp : box.editing.onWindowsWord ? Words.trustWindowsWord : Words.trustThisCertificate
        underWay: box.editing.busy && box.trusting
        Keys.onPressed: event => event.accepted = true
        Keys.onReleased: event => event.accepted = true
        onClicked: {
            if (box.differing) {
                box.editing.typeWatches(noSignal.text, unchanged.text);
                box.editing.saveKnownByWhatIsShown(name.text, address.text, user.text, password.text, group.text);
            } else {
                box.editing.trust();
            }
        }
    }

    // The program is told whether the two are looked at: the box is open on them, in a window
    // that is the active one (one that is out of sight, or made small, is not). The moment after
    // which a yes is taken runs only then, and from the beginning each time: with somebody
    // gone to another program it would pass unseen, and the click that brings the window back
    // lands on whatever is under it.
    Binding {
        target: box.editing
        property: "lookedAt"
        value: box.opened && box.comparing && certificates.Window.active
    }

    Connections {
        target: box.editing

        // The program opens the box, for the KVM it has just put there.
        function onOpened() {
            box.fill();
            box.open();
            box.settle();
        }
        // The box stays, and the KVM is another than it showed (it was saved, or another window
        // changed it): what was typed is gone, the password too, and the KVM as it is stands in
        // the lines.
        function onFilledAnew() {
            box.fill();
            box.settle();
        }
        // The address that was asked is another than the one typed (a JetKVM, asked both ways): its line
        // says that one from here on, as it is typed, so that a yes the box then gives goes with it.
        function onAddressRetyped(at: string) {
            address.text = at;
        }
        // The program closes the box: its window is out of sight.
        function onCloseAsked() {
            box.close();
        }
        // While Save says it is checking, its sign-in waits for an earlier one to that KVM to be
        // answered (another window's, the wall's own): said plainly, in this window's corner, where
        // nothing of the box moves for it.
        function onSignInWaitsForAnEarlierOne() {
            box.actions.tell(Words.waitsForAnEarlierSignIn, false);
        }
        // It is done: the box closes, and what came of it is told in this window's corner, as the
        // page tells it. (A KVM that was taken off is seen to be gone.)
        function onDone(what: string, called: string) {
            box.close();
            if (what === "saved")
                box.actions.tell(Words.savedNamed.arg(called), true);
            else if (what === "trusted")
                box.actions.tell(Words.certificateTrusted.arg(called), true);
        }
        // It was done in the very turn in which the box was closed from outside (its window went
        // out of sight, or the wall was left): the box says nothing any more, so it is told in
        // the window's corner, where it stays until somebody sees the window. With what the
        // box would have stayed open to show (`known`): what the KVM is known by now, or that
        // it is spoken to in the clear.
        function onDoneAsItClosed(what: string, called: string, known: bool, certificate: string, plain: bool) {
            const done = what === "trusted" ? Words.certificateTrusted.arg(called) : Words.savedNamed.arg(called);
            const knownBy = plain ? Words.spokenToOverPlainHttp : certificate !== "" ? Words.rememberedSha256.arg(certificate) : Words.noCertificateRememberedYet;
            // (The second into its place first, and each as it is: a KVM may be called "%2".)
            box.actions.tellWhenSeen(known ? Words.oneThenAnother.replace("%2", () => knownBy).replace("%1", () => done) : done, true);
        }
        // What was asked is over, and the box is still there: the keyboard is back where it was.
        function onChanged() {
            if (box.asking && !box.editing.busy)
                box.settle();
            box.asking = box.editing.busy;
        }
    }

    Column {
        id: holds

        width: box.availableWidth
        spacing: 14
        // (As high as it is while its KVM is there: so it stays once that went.)
        onHeightChanged: {
            if (!box.goneShown)
                box.heldHeight = height;
        }

        Item {
            width: parent.width
            height: lines.height

            Flickable {
                id: lines

                objectName: "editLines"
                width: parent.width
                // (Where its KVM went: what is left of the height it held, beside what came of it,
                // where that is said under it, and the buttons; held against the box's own changes
                // only, and never higher than the window has room for.)
                height: box.goneShown ? Math.min(Math.max(0, box.heldHeight - (said.visible ? said.height + holds.spacing : 0) - holds.spacing - Theme.controlHeight), box.room) : Math.min(pages.height, box.room)
                contentHeight: pages.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: pages

                    width: lines.width

                    // The KVM as it is on the wall, to be typed over.
                    Column {
                        id: typed

                        width: parent.width
                        visible: box.typing
                        spacing: 14

                        Field {
                            id: name

                            objectName: "editName"
                            width: parent.width
                            enabled: !box.editing.busy && !box.gone
                            label: Words.name
                            // (One that has no name is called by where it is, and stays so.)
                            placeholder: box.editing.called
                            onAccepted: box.submit()
                            onHasKeyboardChanged: box.bringIntoSight(name)
                        }
                        Field {
                            id: address

                            objectName: "editAddress"
                            width: parent.width
                            enabled: !box.editing.busy && !box.gone
                            label: Words.address
                            placeholder: "192.168.1.20"
                            onAccepted: box.submit()
                            onHasKeyboardChanged: box.bringIntoSight(address)
                        }
                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            text: box.editing.family === "jetkvm" ? Words.typeOfAJetKvm : box.editing.family === "redfish" ? Words.typeOfAServer : Words.typeOfAKvm
                            color: Theme.muted
                            font.pixelSize: Theme.smallSize
                            wrapMode: Text.WordWrap
                        }
                        // (A JetKVM has one password and no user name: what stands here for it is kept
                        // as it is, and not shown.)
                        Field {
                            id: user

                            objectName: "editUser"
                            width: parent.width
                            visible: box.editing.family !== "jetkvm"
                            enabled: !box.editing.busy && !box.gone
                            label: Words.userName
                            placeholder: "admin"
                            onAccepted: box.submit()
                            onHasKeyboardChanged: box.bringIntoSight(user)
                        }
                        Field {
                            id: password

                            objectName: "editPassword"
                            width: parent.width
                            enabled: !box.editing.busy && !box.gone
                            label: Words.password
                            placeholder: Words.unchanged
                            secret: true
                            onAccepted: box.submit()
                            onHasKeyboardChanged: box.bringIntoSight(password)
                        }
                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            text: Words.leaveEmptyToKeep
                            color: Theme.muted
                            font.pixelSize: Theme.smallSize
                            wrapMode: Text.WordWrap
                        }
                        Field {
                            id: group

                            objectName: "editGroup"
                            width: parent.width
                            enabled: !box.editing.busy && !box.gone
                            label: Words.group
                            placeholder: Words.none
                            onAccepted: box.submit()
                            onHasKeyboardChanged: box.bringIntoSight(group)
                        }
                        // Its screen watches (minutes; empty is off), where its card shows a screen.
                        Row {
                            width: parent.width
                            spacing: 12
                            visible: box.editing.watchable
                            Field {
                                id: noSignal

                                objectName: "editNoSignal"
                                width: (parent.width - parent.spacing) / 2
                                enabled: !box.editing.busy && !box.gone
                                label: Words.noSignalFor
                                placeholder: Words.watchOff
                                validator: IntValidator {
                                    bottom: 0
                                    top: 1440
                                }
                                onAccepted: box.submit()
                                onHasKeyboardChanged: box.bringIntoSight(noSignal)
                            }
                            Field {
                                id: unchanged

                                objectName: "editUnchanged"
                                width: (parent.width - parent.spacing) / 2
                                enabled: !box.editing.busy && !box.gone
                                label: Words.screenUnchangedFor
                                placeholder: Words.watchOff
                                validator: IntValidator {
                                    bottom: 0
                                    top: 1440
                                }
                                onAccepted: box.submit()
                                onHasKeyboardChanged: box.bringIntoSight(unchanged)
                            }
                        }
                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            visible: box.editing.watchable
                            text: Words.noSignalHint + " " + Words.unchangedHint
                            color: Theme.muted
                            font.pixelSize: Theme.smallSize
                            wrapMode: Text.WordWrap
                        }
                        // What it is known by: its certificate, or nothing at all where it is spoken
                        // to in the clear, which is said as what it is. (After a saving that changed
                        // either, it is said under the lines, where it is in sight, and once.)
                        Text {
                            textFormat: Text.PlainText
                            objectName: "editCertificate"
                            width: parent.width
                            visible: box.editing.https && !said.saved
                            text: box.editing.certificate !== "" ? Words.rememberedSha256.arg(box.editing.certificate) : Words.noCertificateRememberedYet
                            color: Theme.muted
                            font.pixelSize: Theme.smallSize
                            wrapMode: Text.WrapAnywhere
                        }
                        Text {
                            textFormat: Text.PlainText
                            objectName: "editPlain"
                            width: parent.width
                            visible: box.editing.overPlainHttp && !said.saved
                            text: Words.spokenToOverPlainHttp
                            color: Theme.warn
                            font.pixelSize: Theme.smallSize
                            wrapMode: Text.WordWrap
                        }
                    }
                    // Before it is taken off the wall: what that does, and what it does not. Or, in
                    // its place, that it was taken off this wall meanwhile.
                    Text {
                        textFormat: Text.PlainText
                        objectName: "removeWarning"
                        width: parent.width
                        visible: box.removing && !box.comparing && !box.goneShown
                        text: Words.whatRemovingDoes
                        color: Theme.text
                        font.pixelSize: Theme.textSize
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        textFormat: Text.PlainText
                        objectName: "removeGone"
                        width: parent.width
                        visible: box.goneAtTheQuestion
                        text: Words.takenOffMeanwhile.arg(box.editing.calledInASentence)
                        color: Theme.warn
                        font.pixelSize: Theme.textSize
                        // (Between words, and inside a name that has no blank in it where it is
                        // longer than a line, as a note has it: it never stands over the edge.)
                        wrapMode: Text.Wrap
                    }
                    // The certificate it is known by, and the one it shows right now; or what
                    // it is known by, and what it would be known by at the address that was
                    // typed for it.
                    Column {
                        id: certificates

                        objectName: "certificates"
                        width: parent.width
                        visible: box.comparing
                        spacing: 14

                        Row {
                            id: both

                            width: parent.width
                            spacing: 14

                            Fingerprint {
                                objectName: "certificateRemembered"
                                anchors.bottom: parent.bottom
                                width: (both.width - both.spacing) / 2
                                label: Words.rememberedByQkvm
                                // (Before the two were read: the one the wall has for it. And
                                // one that is spoken to over HTTPS and known by no certificate
                                // is known by Windows' word: said as that where it is to be
                                // given up, for plain HTTP or for a certificate shown elsewhere.)
                                written: box.editing.reviewed || box.differing ? box.editing.remembered : box.editing.certificate
                                instead: box.editing.plainHttpThere || (box.differing && box.editing.https) ? Words.windowsWordForItsCertificate : Words.noneYet
                            }
                            Fingerprint {
                                objectName: "certificatePresented"
                                anchors.bottom: parent.bottom
                                width: (both.width - both.spacing) / 2
                                visible: box.editing.reviewed || box.differing || box.editing.busy
                                // (Where it was shown is written out: not where the KVM is on the
                                // wall, but at the address that was typed for it. And where
                                // Windows trusts what is shown there, the KVM would be known by
                                // that word and by no certificate: said in the place of one.
                                // Where that address is plain HTTP nothing is presented at all:
                                // what stands there is what is given up, and it says where.)
                                label: box.editing.plainHttpThere ? "" : box.differing ? Words.presentedAtTheNewAddress.arg(box.editing.newAddress) : Words.presentedRightNow
                                written: box.editing.onWindowsWord || box.editing.plainHttpThere ? "" : box.editing.presented
                                instead: box.editing.plainHttpThere ? Words.plainHttpThereShowsNothing.arg(box.editing.newAddress) : box.editing.onWindowsWord ? Words.windowsTrustsWhatIsShownThere : Words.checking
                                grave: box.editing.plainHttpThere
                                // (It is another than the one a yes was given to.)
                                marked: box.editing.yesWasToAnother
                            }
                        }
                        Text {
                            textFormat: Text.PlainText
                            objectName: "certificatesMatch"
                            width: parent.width
                            visible: box.editing.reviewed && box.editing.same
                            text: Words.theyMatch
                            color: Theme.ok
                            font.pixelSize: Theme.textSize
                            wrapMode: Text.WordWrap
                        }
                        // A yes was given, and what is shown now is another than the one it was
                        // given to: said so that it is not missed, in the place of the note
                        // that was read before. Where it is Windows' word that a yes would be
                        // said to now, what that means is said with it.
                        Text {
                            textFormat: Text.PlainText
                            objectName: "certificateAnotherNow"
                            width: parent.width
                            visible: box.editing.yesWasToAnother && box.offersAYes
                            text: box.saidOfAnother
                            color: Theme.warn
                            font.pixelSize: Theme.textSize
                            font.weight: Font.DemiBold
                            wrapMode: Text.WordWrap
                        }
                        // (Of plain HTTP everything is said where a certificate would stand.)
                        Note {
                            objectName: "certificateWarning"
                            width: parent.width
                            visible: box.isAnothers || (box.offersAYes && !box.editing.yesWasToAnother && !box.editing.plainHttpThere)
                            tone: "warn"
                            text: box.saidOfTheTwo
                        }
                    }
                }
            }
            // Where one is in what is longer than its place: a thin mark beside it, in the box's
            // own margin, where it lies over nothing.
            Rectangle {
                objectName: "editScrollMark"
                visible: lines.contentHeight > lines.height + 1
                x: parent.width + 7
                y: lines.visibleArea.yPosition * lines.height
                width: 3
                height: Math.max(16, lines.visibleArea.heightRatio * lines.height)
                radius: 1.5
                color: Theme.lineStrong
            }
        }
        // Why what was asked was not done: said where it was asked. Or that it was saved, where
        // the box stays for what is to be seen of that: what the KVM is known by from now on, to
        // be compared with what the KVM says of itself, or that it is spoken to in the clear. Or,
        // in the place of either, that the KVM was taken off this wall meanwhile (but at the
        // question, where that stands in the place of its words).
        Column {
            id: said

            // (What stood here until its KVM went stands until that is shown. Of what it was asking
            // when the KVM went nothing more is said: the program keeps no problem of it.)
            readonly property bool refused: !box.goneShown && box.editing.problem !== "" && box.askedHere
            readonly property bool saved: !box.goneShown && box.editing.savedAs !== "" && box.typing
            readonly property bool gone: box.goneShown && !box.goneAtTheQuestion

            width: parent.width
            visible: refused || saved || gone

            Text {
                textFormat: Text.PlainText
                objectName: "editGone"
                width: parent.width
                visible: said.gone
                text: Words.takenOffMeanwhile.arg(box.editing.calledInASentence)
                color: Theme.warn
                font.pixelSize: Theme.textSize
                // (As at the question: a name with no blank in it is broken where it must be.)
                wrapMode: Text.Wrap
            }
            Text {
                textFormat: Text.PlainText
                objectName: "editProblem"
                width: parent.width
                visible: said.refused
                text: box.editing.problem
                color: Theme.bad
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
            // It says by its number that it is another KVM of the wall: the person may keep both, as on the page.
            Button {
                objectName: "editKeepBoth"
                visible: said.refused && box.editing.mayKeepBoth && !box.editing.busy
                text: Words.keepBoth
                onClicked: box.keepBoth()
            }
            Text {
                textFormat: Text.PlainText
                objectName: "editSaved"
                width: parent.width
                visible: said.saved
                text: Words.savedNamed.arg(box.editing.savedAs)
                color: Theme.ok
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
            Text {
                textFormat: Text.PlainText
                objectName: "editSavedPlain"
                width: parent.width
                topPadding: 4
                visible: said.saved && box.editing.overPlainHttp
                text: Words.spokenToOverPlainHttp
                color: Theme.warn
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WordWrap
            }
            Text {
                textFormat: Text.PlainText
                objectName: "editSavedOverHttps"
                width: parent.width
                topPadding: 4
                visible: said.saved && box.editing.ontoHttps
                text: Words.answersOverHttpsNow
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WordWrap
            }
            Text {
                textFormat: Text.PlainText
                objectName: "editSavedCertificate"
                width: parent.width
                topPadding: 4
                visible: said.saved && box.editing.https
                text: box.editing.certificate !== "" ? Words.rememberedSha256.arg(box.editing.certificate) : Words.noCertificateRememberedYet
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WrapAnywhere
            }
        }
        Item {
            width: parent.width
            height: Theme.controlHeight

            Button {
                objectName: "editRemove"
                anchors.left: parent.left
                visible: box.typing
                danger: true
                enabled: !box.editing.busy && !box.gone
                text: Words.remove
                onClicked: box.removing = true
            }
            Row {
                anchors.right: parent.right
                spacing: 8

                // Under the lines.
                Asking {
                    objectName: "editReview"
                    visible: box.typing && box.editing.https
                    enabled: !box.editing.busy && !box.gone
                    says: Words.reviewCertificate
                    underWay: box.editing.busy && box.editing.asked === "looking"
                    onClicked: box.editing.review()
                }
                Button {
                    id: editCancel

                    objectName: "editCancel"
                    visible: box.typing
                    // While something is asked it gives that up, and the box stays. After a saving
                    // that left the box open there is nothing to take back. (Where its KVM went, it
                    // closes the box.) As wide with either word: the buttons beside it stay where
                    // they are when one turns into the other.
                    text: said.saved ? Words.done : Words.cancel
                    width: Math.ceil(Math.max(cancelWidth.advanceWidth, doneWidth.advanceWidth)) + leftPadding + rightPadding
                    onClicked: box.editing.busy ? box.editing.giveUp() : box.close()

                    TextMetrics {
                        id: cancelWidth

                        font: editCancel.font
                        text: Words.cancel
                    }
                    TextMetrics {
                        id: doneWidth

                        font: editCancel.font
                        text: Words.done
                    }
                }
                Asking {
                    objectName: "editSave"
                    visible: box.typing
                    primary: true
                    enabled: !box.editing.busy && !box.gone
                    says: Words.save
                    underWay: box.editing.busy && box.editing.asked === "saving"
                    onClicked: box.submit()
                }

                // Under the question. (Back to the lines; or out, from a box that was opened for the
                // question alone, and from one whose KVM went.)
                Button {
                    id: removeNo

                    objectName: "removeNo"
                    visible: box.removing && !box.comparing
                    text: Words.cancel
                    onClicked: {
                        if (box.editing.removesOnly || box.gone)
                            box.close();
                        else
                            box.removing = false;
                    }
                }
                Button {
                    objectName: "removeYes"
                    visible: box.removing && !box.comparing
                    danger: true
                    enabled: !box.editing.busy && !box.gone
                    text: Words.remove
                    onClicked: box.editing.remove()
                }

                // Under the two certificates. The way back gives up a yes that is being asked;
                // otherwise it leads to the lines, where what was typed stands as it was, or out
                // of a box that was opened for the certificate alone, or whose KVM went. (The yes
                // is not in this row: see above. Where there is none to give, this only closes.)
                Button {
                    id: reviewClose

                    objectName: "reviewClose"
                    visible: box.comparing
                    text: box.editing.busy || box.offersAYes ? Words.cancel : Words.close
                    onClicked: {
                        if (box.editing.busy && box.trusting)
                            box.editing.giveUp();
                        else if (box.editing.forReview || box.gone)
                            box.close();
                        else
                            box.editing.closeReview();
                    }
                }
            }
        }
    }
}
