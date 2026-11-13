import QtQuick
import Qkvm

// Before there is a wall: which QKVM, whether it is the right one, and then who. The address first.
// Then, where the server shows a certificate Windows does not know, that certificate, to be compared
// with what QKVM printed when it started. Then what the server asks for, with the page's words: the
// name where it has more than one account, the password, and the code where the account has a
// second step.
Item {
    id: screen

    required property SessionView session

    readonly property bool trusting: session.stage === "trust"
    readonly property bool signingIn: session.stage === "signIn"
    readonly property bool naming: !trusting && !signingIn

    // The keyboard is where the next thing is typed. A certificate is not said yes to with a key
    // that was meant for something else: while one is shown, the keyboard is nowhere.
    function takeFocus() {
        if (!visible)
            return;
        // (To the screen itself while a certificate is shown, and not to the button that says yes:
        // a key pressed for something else would press it.)
        if (trusting)
            screen.forceActiveFocus();
        else if (naming)
            address.takeFocus();
        else if (session.asksName && name.text === "")
            name.takeFocus();
        else if (session.asksCode)
            code.takeFocus();
        else
            password.takeFocus();
    }

    // What Enter and the button do: ask the server, unless it is being asked or nothing was typed.
    function submit() {
        // (What is sent is on show no longer, as on the page.)
        password.conceal();
        if (session.busy || trusting)
            return;
        if (naming) {
            if (address.text.trim() !== "") {
                // A display link's key does not stay on show for whoever walks past the screen.
                const typed = address.text;
                address.text = typed.split("#")[0];
                session.connectTo(typed);
            }
        } else if (password.text !== "") {
            session.signIn(name.text, password.text, code.text, keep.checked);
        } else {
            password.takeFocus();
        }
    }

    // The server that was used last stands in the address's line until something else is typed there.
    function suggest() {
        if (address.text === "" && session.suggested !== "")
            address.text = session.suggested;
    }

    onVisibleChanged: takeFocus()
    Component.onCompleted: {
        suggest();
        takeFocus();
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(380, parent.width - 32)
        height: form.implicitHeight + 44
        radius: 6
        color: Theme.panel
        border.width: 1
        border.color: Theme.lineStrong

        Column {
            id: form

            x: 22
            y: 22
            width: parent.width - 44
            spacing: 14

            Row {
                spacing: 10

                Mark {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 26
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    text: Words.appName
                    color: Theme.text
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
            }

            // Which QKVM.
            Field {
                id: address

                width: parent.width
                visible: screen.naming
                label: Words.addressOfYourQkvm
                placeholder: "192.168.1.10:8787"
                onAccepted: screen.submit()
                // Somebody is here and types: a server that was being asked again by itself is
                // asked no more, and no wall comes up under what is being typed.
                onEdited: {
                    if (screen.session.tryingAgain)
                        screen.session.stopTrying();
                }
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: screen.naming
                text: Words.orADisplayLink
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WordWrap
            }

            // Whether it is the right one.
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: screen.trusting
                text: screen.session.certificateBefore === "" ? Words.isThisYourQkvm : Words.anotherCertificate
                color: Theme.text
                font.pixelSize: Theme.titleSize
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: screen.trusting
                text: (screen.session.certificateBefore === "" ? Words.certificateNotKnown : Words.certificateNotTheSame).arg(screen.session.address)
                color: screen.session.certificateBefore === "" ? Theme.text : Theme.warn
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
            Fingerprint {
                objectName: "certificate"
                visible: screen.trusting
                label: screen.session.certificateBefore === "" ? Words.itsCertificate : Words.itShowsNow
                written: screen.session.certificate
            }
            Fingerprint {
                objectName: "certificateBefore"
                visible: screen.trusting && screen.session.certificateBefore !== ""
                label: Words.itWasKnownBy
                written: screen.session.certificateBefore
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: screen.trusting
                text: Words.compareWithQkvm
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WordWrap
            }

            // Who.
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: screen.signingIn
                text: Words.signInTo.arg(screen.session.address)
                color: Theme.text
                font.pixelSize: Theme.titleSize
                font.weight: Font.DemiBold
                wrapMode: Text.WrapAnywhere
            }
            Text {
                textFormat: Text.PlainText
                objectName: "notEncrypted"
                width: parent.width
                visible: screen.signingIn && !screen.session.encrypted
                text: Words.notEncrypted
                color: Theme.warn
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WordWrap
            }
            // What the QKVM said of itself before anybody was asked for a password.
            Text {
                textFormat: Text.PlainText
                objectName: "locked"
                width: parent.width
                visible: screen.signingIn && screen.session.locked
                text: Words.restartedLocked
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WordWrap
            }
            Field {
                id: name

                width: parent.width
                visible: screen.signingIn && screen.session.asksName
                label: Words.userName
                onAccepted: password.takeFocus()
            }
            Field {
                id: password

                objectName: "password"
                width: parent.width
                visible: screen.signingIn
                label: Words.password
                secret: true
                onAccepted: screen.submit()
            }
            Field {
                id: code

                width: parent.width
                visible: screen.signingIn && screen.session.asksCode
                label: Words.codeFromYourApp
                onAccepted: screen.submit()
            }
            Check {
                id: keep

                objectName: "keep"
                width: parent.width
                visible: screen.signingIn
                // A program on one's own PC stays signed in unless it is told not to.
                checked: true
                text: Words.keepSignedIn
            }

            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: screen.session.problem !== ""
                text: screen.session.problem
                color: Theme.bad
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
            // A screen nobody sits at, whose QKVM does not answer yet: it goes on asking.
            Text {
                textFormat: Text.PlainText
                objectName: "tryingAgain"
                width: parent.width
                visible: screen.naming && screen.session.tryingAgain
                text: Words.tryingAgainByItsLink
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WordWrap
            }

            Item {
                width: parent.width
                height: Theme.controlHeight

                Button {
                    objectName: "ownWall"
                    anchors.left: parent.left
                    visible: screen.naming
                    ghost: true
                    text: Words.thisPcsOwnKvms
                    enabled: !screen.session.busy
                    onClicked: screen.session.goToOwnWall()
                }
                Button {
                    anchors.left: parent.left
                    visible: !screen.naming
                    ghost: true
                    text: Words.anotherQkvm
                    enabled: !screen.session.busy
                    onClicked: screen.session.anotherServer()
                }
                Button {
                    anchors.right: parent.right
                    visible: screen.naming
                    primary: true
                    text: Words.connect
                    enabled: !screen.session.busy
                    onClicked: screen.submit()
                }
                Button {
                    id: trustButton

                    objectName: "trust"
                    anchors.right: parent.right
                    visible: screen.trusting
                    // The first time it is the thing to do. Later it is the exception, and looks it.
                    primary: screen.session.certificateBefore === ""
                    text: screen.session.certificateBefore === "" ? Words.trustThisCertificate : Words.trustTheNewCertificate
                    onClicked: screen.session.trustCertificate()
                }
                Button {
                    anchors.right: parent.right
                    visible: screen.signingIn
                    primary: true
                    text: Words.signIn
                    enabled: !screen.session.busy
                    onClicked: screen.submit()
                }
            }
        }
    }

    // A password is not kept once it was used, nor for another server.
    Connections {
        function onChanged() {
            if (screen.session.stage !== "signIn") {
                password.text = "";
                code.text = "";
            }
            screen.suggest();
            if (!screen.session.busy)
                screen.takeFocus();
        }

        target: screen.session
    }

    // A certificate's SHA-256 under what it is, in letters that are all of one width.
    component Fingerprint: Column {
        id: fingerprint

        property string label
        property string written

        // In four lines of eight, as it is easiest to compare by eye.
        readonly property string inLines: {
            const pairs = written.split(":");
            const lines = [];
            for (let at = 0; at < pairs.length; at += 8)
                lines.push(pairs.slice(at, at + 8).join(":"));
            return lines.join("\n");
        }

        width: parent.width
        spacing: 5

        Text {
            textFormat: Text.PlainText
            text: fingerprint.label
            color: Theme.muted
            font.pixelSize: Theme.headingSize
            font.weight: Font.DemiBold
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 0.9
        }
        Rectangle {
            width: parent.width
            height: letters.implicitHeight + 16
            radius: Theme.radius
            color: Theme.inset
            border.width: 1
            border.color: Theme.lineStrong

            Text {
                id: letters

                textFormat: Text.PlainText
                anchors.centerIn: parent
                text: fingerprint.inLines
                color: Theme.text
                font.family: "Consolas"
                font.pixelSize: Theme.textSize
                lineHeight: 1.25
                Accessible.name: fingerprint.label + ": " + fingerprint.written
            }
        }
    }
}
