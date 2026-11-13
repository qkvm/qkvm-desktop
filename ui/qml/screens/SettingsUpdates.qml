import QtQuick
import Qkvm

Section {
    id: section

    required property UpdatesView updates

    objectName: "settingsUpdates"
    name: qsTr("Updates")
    about: section.updates !== null && section.updates.showsFile ? qsTr("Check for a new version of qKVM. You choose when to download it, and unpack it yourself.") : section.updates !== null && section.updates.restarts ? qsTr("Check for a new version of qKVM. You choose when to download it, and when qKVM restarts to install it.") : qsTr("Check for a new version of qKVM. You choose when to download and install it.")
    does: Button {
        objectName: "checkForUpdates"
        text: qsTr("Check for updates")
        enabled: section.updates !== null && section.updates.canCheck
        onClicked: section.updates.check()
    }

    Block {
        title: qsTr("This version")

        Line {
            label: qsTr("Installed version")
            value: section.updates !== null ? section.updates.currentVersion : ""
        }
        Line {
            label: qsTr("Last successful check")
            value: section.updates !== null ? section.updates.lastChecked : qsTr("Never")
        }
        Line {
            label: qsTr("Check automatically")
            hint: qsTr("At startup and once a day while qKVM is running. Installation always waits for you.")

            Check {
                objectName: "checkUpdatesAutomatically"
                checked: section.updates !== null && section.updates.automatic
                enabled: section.updates !== null
                Accessible.name: qsTr("Check automatically")
                onToggled: section.updates.automatic = checked
            }
        }
    }
    Block {
        title: qsTr("Update status")

        Line {
            objectName: "afterSetup"
            visible: section.updates !== null && section.updates.afterSetup !== ""
            label: section.updates !== null ? section.updates.afterSetup : ""
            tone: section.updates !== null && section.updates.afterSetupWentWrong ? "bad" : ""
        }
        Line {
            objectName: "updateStatus"
            label: section.updates !== null ? section.updates.status : qsTr("Updates are not available for this build.")
            hint: section.updates !== null ? section.updates.details : ""

            Button {
                objectName: "downloadUpdate"
                visible: section.updates !== null && section.updates.canDownload
                text: qsTr("Download update")
                onClicked: section.updates.download()
            }
            Button {
                objectName: "installUpdate"
                visible: section.updates !== null && section.updates.ready && !section.updates.showsFile
                text: section.updates !== null && section.updates.restarts ? qsTr("Install and restart") : qsTr("Install update...")
                onClicked: section.updates.install()
            }
            Button {
                objectName: "showUpdate"
                visible: section.updates !== null && section.updates.ready && section.updates.showsFile
                text: qsTr("Show the verified file")
                onClicked: section.updates.showFile()
            }
            Button {
                objectName: "cancelUpdate"
                visible: section.updates !== null && (section.updates.busy || section.updates.ready)
                text: section.updates !== null && section.updates.ready ? qsTr("Later") : qsTr("Cancel")
                onClicked: section.updates.cancel()
            }
        }
        Line {
            visible: section.updates !== null && section.updates.ready && !section.updates.showsFile && !section.updates.restarts
            label: qsTr("Save your work before installing")
            hint: qsTr("qKVM Setup will ask you to confirm. Quit qKVM from its tray before continuing in Setup. Open it again after installation. A previous verified version is kept for recovery.")
        }
        Line {
            visible: section.updates !== null && section.updates.ready && section.updates.restarts
            label: qsTr("qKVM closes to install it")
            hint: qsTr("qKVM Setup installs the update into the folder qKVM runs from, and starts qKVM again. Finish control, actions and file transfers in every window first. Your machines and settings stay on this PC.")
        }
        Line {
            visible: section.updates !== null && section.updates.ready && section.updates.showsFile
            label: qsTr("Unpack it into a new folder")
            hint: qsTr("Quit qKVM from its tray, unpack the file into a folder of your choice, and start qKVM from there. Your machines and settings stay on this PC.")
        }
        Line {
            visible: section.updates !== null && section.updates.notes !== ""
            label: qsTr("What's new")
            hint: section.updates !== null ? section.updates.notes : ""
        }
        Line {
            visible: section.updates !== null && section.updates.problem !== ""
            label: section.updates !== null ? section.updates.problem : ""
            tone: "bad"
        }
    }
    Block {
        title: qsTr("Recovery")
        visible: section.updates !== null && section.updates.canRecover

        Line {
            label: qsTr("Return to the previous version")
            hint: qsTr("Uses the previous verified installer kept on this PC. Your saved machines and settings stay in place. Setup asks before making changes.")

            Button {
                objectName: "rollbackUpdate"
                text: qsTr("Previous version...")
                enabled: section.updates !== null && !section.updates.busy
                onClicked: section.updates.recover(true)
            }
        }
        Line {
            label: qsTr("Recover an interrupted installation")
            hint: qsTr("Checks the saved installation record and either completes the update or restores the previous version.")

            Button {
                objectName: "recoverUpdate"
                text: qsTr("Recover installation...")
                enabled: section.updates !== null && !section.updates.busy
                onClicked: section.updates.recover(false)
            }
        }
    }
}
