import QtQuick
import Qkvm

// The settings' section of a qKVM server's wall as a file, for whoever manages it: the page's
// "Export and import" in the page's words (app.js, filesTab). The server writes the file and reads
// one: "Export..." and "Import..." in its first line open the two boxes that do it there
// (ExportBox.qml, ImportBox.qml), over the settings.
Section {
    id: section

    signal exportAsked
    signal importAsked

    objectName: "settingsServerFiles"
    name: Words.exportAndImport
    about: Words.theWallAsAFileAbout

    does: [
        Button {
            objectName: "serverExport"
            symbol: "download"
            text: Words.exportDots
            onClicked: section.exportAsked()
        },
        Button {
            objectName: "serverImport"
            primary: true
            symbol: "upload"
            text: Words.importDots
            onClicked: section.importAsked()
        }
    ]

    Intro {
        text: Words.theWallAsAFile
    }
    Block {
        title: Words.fileIsOneOfTwoKinds

        Line {
            label: Words.encryptedFile
            hint: Words.serverEncryptedFileHolds
        }
        Line {
            label: Words.plainFile
            hint: Words.serverPlainFileHolds
        }
    }
    Block {
        title: Words.takingAFileIn

        Line {
            label: Words.addsWhatIsNotHereYet
            hint: Words.serverAddsWhatIsNotHereYetHow
        }
    }
    Footnote {
        text: Words.whatAServersFileHolds
    }
}
