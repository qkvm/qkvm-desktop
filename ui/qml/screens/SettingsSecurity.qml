import QtQuick
import Qkvm

Section {
    id: section
    required property AppLock security
    objectName: "settingsSecurity"
    name: qsTr("App lock")
    about: qsTr("One app password protects actions and saved secrets in every qKVM window on this PC.")
    Block {
        title: qsTr("Access on this PC")
        Line {
            label: qsTr("Sign in automatically on this PC")
            hint: qsTr("When off, qKVM asks for the app password at every start. Locking always requires the password again, even after a restart.")
            Check {
                objectName: "automaticAppSignIn"
                Accessible.name: qsTr("Sign in automatically on this PC")
                checked: section.security !== null && section.security.automatic
                enabled: section.security !== null && !section.security.restricted && !section.security.busy
                onToggled: {
                    if (checked)
                        section.security.ask("automatic");
                    else
                        section.security.disableAutomatic();
                    checked = Qt.binding(() => section.security !== null && section.security.automatic);
                }
            }
        }
        Line {
            label: qsTr("App password")
            hint: qsTr("Required to unlock, change the password, sign out a wall screen, or export saved secrets.")
            Button {
                text: qsTr("Change password")
                enabled: section.security !== null && !section.security.restricted
                onClicked: section.security.ask("password")
            }
            Button {
                text: qsTr("Lock now")
                enabled: section.security !== null && !section.security.locked
                onClicked: section.security.lock()
            }
        }
        Line {
            visible: section.security !== null && section.security.wallScreen
            label: qsTr("This PC is a wall screen")
            hint: qsTr("Sign out with the local app password, even when the server is unavailable.")
            Button {
                text: qsTr("Sign out wall screen")
                onClicked: section.security.ask("endScreen")
            }
        }
    }
}
