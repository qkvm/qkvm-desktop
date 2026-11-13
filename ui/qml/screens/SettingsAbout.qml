import QtQuick
import Qkvm

// The settings' section About: what the About box shows (AboutBlocks.qml, one for both).
Section {
    id: section

    // What there is to say of the program. (Not "about": that is the section's sentence.)
    required property AboutModel program

    objectName: "settingsAbout"
    name: Words.about
    about: Words.aboutAbout

    AboutBlocks {
        about: section.program
    }
}
