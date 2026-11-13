import QtQuick
import Qkvm

// The settings' section of this PC's wall as a file, the page's "Export and import", in this
// program's words: what is in a file of the wall of either kind, and what taking one in does.
// "Export..." and "Import..." in its first line open the two boxes that do it (OwnExportBox.qml,
// OwnImportBox.qml), over the settings.
Section {
    id: section

    signal exportAsked
    signal importAsked

    objectName: "settingsFiles"
    name: Words.exportAndImport
    about: Words.exportAndImportAbout

    does: [
        Button {
            objectName: "settingsExport"
            symbol: "download"
            text: Words.exportDots
            onClicked: section.exportAsked()
        },
        Button {
            objectName: "settingsImport"
            primary: true
            symbol: "upload"
            text: Words.importDots
            onClicked: section.importAsked()
        }
    ]

    Intro {
        text: Words.thisPcsWallAsAFile
    }
    Block {
        title: Words.fileIsOneOfTwoKinds

        Line {
            label: Words.encryptedFile
            hint: Words.encryptedFileHolds
        }
        Line {
            label: Words.plainFile
            hint: Words.plainFileHolds
        }
    }
    Block {
        title: Words.takingAFileIn

        Line {
            label: Words.addsWhatIsNotHereYet
            hint: Words.addsWhatIsNotHereYetHow
        }
        Line {
            label: Words.actionsAlertsAndSettings
            hint: Words.stayInTheFile
        }
    }
    Footnote {
        text: Words.whatAFileOfThisWallHolds
    }
}
