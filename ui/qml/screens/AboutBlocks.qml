pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// What this program is, whose it is, under which licence, and what it runs on: the program and its
// version; where to find it; its licence; and the libraries that ship with it, each with its version,
// its licence and where its source is offered. One for the two that show it, the About box and the
// settings' section About, and made of the shapes every section is made of.
Column {
    id: blocks

    required property AboutModel about

    // Which of the shapes the settings are made of this is: a group of them (Section.qml).
    readonly property string shape: "blocks"

    width: parent ? parent.width : 0
    spacing: 24

    Block {
        Line {
            objectName: "aboutProgram"
            label: Words.appName
            hint: Words.whatThisProgramIs
            value: Words.version.arg(blocks.about.version)
        }
    }
    Block {
        title: Words.whereToFindIt

        Line {
            label: Words.qkvmSite
            hint: Words.website
        }
        Line {
            objectName: "aboutSource"
            label: Words.itsSource
            hint: blocks.about.source
        }
    }
    Block {
        title: Words.licence
        text: Words.freeSoftware.arg(blocks.about.licence)
    }
    Block {
        visible: blocks.about.diagnosticsAvailable
        title: qsTr("Diagnostics")

        Line {
            label: qsTr("Local reports")
            hint: blocks.about.diagnosticsProblem || qsTr("Crash summaries and video diagnostics stay on this PC. Nothing is uploaded automatically.")
            tone: blocks.about.diagnosticsProblem ? "bad" : ""

            Button {
                objectName: "openDiagnostics"
                text: qsTr("Open folder")
                onClicked: blocks.about.openDiagnostics()
            }
        }
    }
    Listed {
        id: libraries

        objectName: "aboutLibraries"
        title: Words.runsOn
        columns: [Words.library, Words.versionOf, Words.licence]
        // Its name, and where its source is, which takes up to three lines; its version and its
        // licence, up to two each.
        lines: [
            {
                "text": 1,
                "under": [3]
            },
            {
                "text": 2
            },
            {
                "text": 2
            }
        ]
        model: blocks.about

        delegate: ListRow {
            required property string name
            required property string version
            required property string licence
            required property string source

            of: libraries
            cells: [
                {
                    "text": name,
                    "under": [Words.sourceAt.arg(source)]
                },
                {
                    "text": version
                },
                {
                    "text": licence
                }
            ]
        }
    }
    Footnote {
        text: Words.theseLibraries
    }
}
