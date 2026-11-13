import QtQuick
import Qkvm

Dialog {
    id: box
    required property AppLock security
    readonly property string mode: security !== null ? security.mode : ""
    readonly property bool creates: mode === "setup" || mode === "password"
    readonly property bool resets: mode === "reset" || mode === "recovery"
    // Windows did not hand over what qKVM keeps: only Try again and Quit, never a reset.
    readonly property bool unavailable: mode === "unavailable"
    readonly property bool inSight: form.inSight
    onInSightChanged: {
        if (!inSight)
            clear();
    }
    width: Math.min(520, parent ? parent.width - 24 : 520)
    title: mode === "setup" ? qsTr("Set the app password") : mode === "password" ? qsTr("Change app password") : mode === "reset" || mode === "recovery" ? qsTr("Reset saved secrets") : mode === "endScreen" ? qsTr("Sign out wall screen") : mode === "export" ? qsTr("Authorize export") : mode === "automatic" ? qsTr("Enable automatic sign-in") : mode === "unavailable" ? qsTr("Saved secrets not available") : qsTr("Unlock qKVM")
    closesBeside: false

    function clear() {
        current.text = "";
        replacement.text = "";
        confirmation.text = "";
        current.conceal();
        replacement.conceal();
        confirmation.conceal();
    }
    function follow() {
        if (mode !== "")
            open();
        else
            close();
    }
    // What the box last showed: where it showed "unavailable", the buttons of what it shows next take no
    // click for a moment (a second click meant for Try again lands on nothing).
    property string shownMode: ""
    readonly property bool settled: !settling.running
    Timer {
        id: settling
        interval: 800
    }
    onModeChanged: {
        if (shownMode === "unavailable" && mode !== "unavailable")
            settling.restart();
        shownMode = mode;
        clear();
        understood.checked = false;
        follow();
    }
    Component.onCompleted: follow()
    onOpened: {
        automatic.checked = true;
        if (mode === "setup")
            replacement.takeFocus();
        else if (!resets && !unavailable)
            current.takeFocus();
    }
    onClosed: {
        clear();
        if (security !== null && mode !== "")
            security.cancel();
    }
    Connections {
        target: box.security
        function onClearFields() {
            box.clear();
        }
    }
    Flickable {
        width: box.availableWidth
        implicitHeight: Math.min(contentHeight, box.parent ? box.parent.height - 100 : 600)
        height: implicitHeight
        contentHeight: form.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Column {
            id: form
            readonly property bool inSight: Window.visibility !== Window.Hidden && Window.visibility !== Window.Minimized
            width: box.availableWidth
            spacing: 12
            Text {
                width: parent.width
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: Theme.text
                visible: !box.unavailable
                text: box.resets ? qsTr("Reset erases every saved KVM password, saved-action text, server session and display link on this PC. Your KVM inventory stays. qKVM closes after the reset; open it again to set a new app password and re-enter the KVM passwords.") : box.mode === "setup" ? qsTr("Choose an app password with 10 to 256 characters. Keep it somewhere safe: forgetting it requires erasing saved secrets.") : box.mode === "export" ? qsTr("Enter the local app password again to export saved secrets.") : qsTr("Enter the app password for this PC.")
            }
            Field {
                id: current
                objectName: "appCurrentPassword"
                width: parent.width
                visible: !box.resets && !box.unavailable && box.mode !== "setup"
                secret: true
                label: qsTr("Current app password")
            }
            Field {
                id: replacement
                objectName: "appNewPassword"
                width: parent.width
                visible: box.creates
                secret: true
                label: qsTr("New app password")
            }
            Field {
                id: confirmation
                objectName: "appConfirmPassword"
                width: parent.width
                visible: box.creates
                secret: true
                label: qsTr("Confirm new password")
            }
            Check {
                id: automatic
                text: qsTr("Sign in automatically on this PC")
                checked: true
                visible: box.mode === "setup"
            }
            Check {
                id: understood
                text: qsTr("Erase all saved secrets and close qKVM")
                visible: box.mode === "reset"
            }
            Text {
                width: parent.width
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: Theme.bad
                text: box.security !== null ? box.security.problem : ""
                visible: text !== ""
            }
            Text {
                textFormat: Text.PlainText
                color: Theme.muted
                text: box.security !== null && box.security.busy ? qsTr("Checking password…") : box.security !== null && box.security.retryAfter > 0 ? qsTr("Try again in %1 seconds.").arg(box.security.retryAfter) : ""
                visible: text !== ""
            }
            Row {
                spacing: 8
                Button {
                    objectName: "appForgotPassword"
                    text: qsTr("Forgot password")
                    visible: !box.resets && !box.unavailable && box.mode !== "setup"
                    enabled: box.settled
                    onClicked: box.security.forgot()
                }
                Button {
                    objectName: "appLockCancel"
                    text: qsTr("Cancel")
                    visible: !box.unavailable
                    enabled: box.settled
                    onClicked: box.security.cancel()
                }
                Button {
                    objectName: "appSecretsQuit"
                    text: qsTr("Quit")
                    visible: box.unavailable
                    onClicked: box.security.quit()
                }
                Button {
                    objectName: "appSecretsTryAgain"
                    primary: true
                    text: qsTr("Try again")
                    visible: box.unavailable
                    onClicked: box.security.tryAgain()
                }
                Button {
                    objectName: "appPasswordSubmit"
                    visible: !box.unavailable
                    primary: true
                    text: box.mode === "recovery" ? qsTr("Review reset") : box.resets ? qsTr("Erase saved secrets") : box.creates ? qsTr("Save password") : qsTr("Continue")
                    enabled: box.settled && box.security !== null && !box.security.busy && (box.mode === "recovery" || (box.resets ? understood.checked : box.security.retryAfter === 0))
                    onClicked: {
                        if (box.mode === "recovery")
                            box.security.forgot();
                        else if (box.resets)
                            box.security.resetSecrets(understood.checked);
                        else
                            box.security.submit(current.text, replacement.text, confirmation.text, automatic.checked);
                    }
                }
            }
        }
    }
}
