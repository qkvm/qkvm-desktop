import QtQuick
import Qkvm

// About: the program and its version, where to find it, its licence, and what ships with it, each
// with its own licence and where its source is offered. What the settings' section About shows as
// well (AboutBlocks.qml, one for both). What it holds scrolls where the window is too low for it.
Dialog {
    id: box

    required property AboutModel about

    title: Words.about
    width: Math.min(560, parent ? parent.width - 24 : 560)

    Item {
        width: box.availableWidth
        implicitHeight: lines.height

        // (Reaching into the box's edge at its right by a little: where what it holds scrolls, the
        // mark that says so stands there, beside the frames and not over them.)
        Flickable {
            id: lines

            objectName: "aboutLines"
            width: parent.width + 12
            height: Math.min(blocks.height, (box.parent ? box.parent.height : 700) - 24 - box.implicitHeaderHeight - box.topPadding - box.bottomPadding)
            contentWidth: width
            contentHeight: blocks.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            AboutBlocks {
                id: blocks

                width: lines.width - 12
                about: box.about
            }
        }
        ScrollMark {
            flickable: lines
        }
    }
}
