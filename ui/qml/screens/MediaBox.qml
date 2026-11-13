pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// A machine's virtual media through its qKVM, as the page's Actions > Media... has it (app.js, mediaDialog): an
// admin's, in person. One box that holds still: one size and place whatever it shows, and only its list of images
// scrolls. Its main button, Mount, is at the right of its first line, with Upload and Delete before it; what is in the
// drive is under it, with Unmount at its right; then what the image goes in as, how the machine is to start, the list,
// and one line for what is said (an upload's progress too). A control that cannot be used is shown disabled, never
// taken away, so that nothing comes to stand where another one stood. Each mount, unmount, upload and deletion is
// asked first, naming the machine and the image. While something of it is out, nothing in it takes a click (its way
// out does: closing it stops an upload, and what else is out goes on and is told in the window's corner).
Dialog {
    id: box

    required property ActionsView actions
    readonly property MediaView media: actions.media
    readonly property var asked: media.question
    // One height, whatever it shows: as high as the window lets it be, up to the page's.
    readonly property real high: Math.min(620, (parent ? parent.height : 640) - 24)

    // One line of the frame under the first: what it is at the left, and what one does about it at the right.
    component Setting: Item {
        id: setting

        property string label
        property string hint
        property string hintName
        default property alias does: atItsRight.data

        width: parent ? parent.width : 0
        height: 52

        Rectangle {
            width: parent.width
            height: 1
            visible: setting.Positioner.index > 0
            color: Theme.line
        }
        Column {
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            width: atItsRight.x - x - 16
            spacing: 2

            Text {
                textFormat: Text.PlainText
                width: parent.width
                text: setting.label
                color: Theme.text
                font.pixelSize: Theme.textSize
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Text {
                textFormat: Text.PlainText
                objectName: setting.hintName
                width: parent.width
                visible: text !== ""
                text: setting.hint
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                elide: Text.ElideRight
            }
        }
        Row {
            id: atItsRight

            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
        }
    }

    title: media.title
    width: Math.min(680, parent ? parent.width - 24 : 680)
    height: high
    closesBeside: !media.busy
    onClosed: media.close()

    // Opened and closed as the program says: its menu opens it, and it goes with whoever may have it.
    readonly property bool there: media.opened

    onThereChanged: {
        if (there)
            open();
        else
            close();
    }

    Item {
        id: inside

        width: box.availableWidth
        // (Under the title's line, which the box's height counts and its available height does not.)
        height: box.high - box.implicitHeaderHeight - box.topPadding - box.bottomPadding
        enabled: !box.media.busy

        // The first line: what the images are and where, and what is done with them at its right.
        Item {
            id: head

            width: parent.width
            height: Theme.controlHeight

            Column {
                anchors.left: parent.left
                anchors.right: does.left
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    textFormat: Text.PlainText
                    text: Words.mediaImages
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    font.weight: Font.DemiBold
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "mediaWhere"
                    width: parent.width
                    text: box.media.where
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    elide: Text.ElideRight
                }
            }
            Row {
                id: does

                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Button {
                    objectName: "mediaUpload"
                    enabled: box.media.canUpload
                    symbol: "upload"
                    text: Words.mediaUpload
                    hint: Words.mediaUploadHint
                    onClicked: box.media.upload()
                }
                Button {
                    objectName: "mediaDelete"
                    enabled: box.media.canDelete
                    symbol: "trash"
                    text: Words.mediaDelete
                    hint: Words.mediaDeleteHint
                    onClicked: box.media.remove()
                }
                Button {
                    objectName: "mediaMount"
                    primary: true
                    enabled: box.media.canMount
                    symbol: "disc"
                    text: Words.mediaMount
                    onClicked: box.media.mount()
                }
            }
        }
        // What is in the drive, with Unmount at its right; what the image goes in as; how the machine is to start.
        Rectangle {
            id: how

            y: head.height + 14
            width: parent.width
            height: lines.height + 2
            radius: Theme.radius
            color: "transparent"
            border.width: 1
            border.color: Theme.line

            Column {
                id: lines

                x: 1
                y: 1
                width: parent.width - 2

                Setting {
                    label: Words.mediaInItsDrive
                    hint: box.media.now
                    hintName: "mediaNow"

                    Button {
                        objectName: "mediaUnmount"
                        enabled: box.media.canUnmount
                        symbol: "eject"
                        text: Words.mediaUnmount
                        onClicked: box.media.unmount()
                    }
                }
                Setting {
                    label: Words.mediaMountAs

                    Choice {
                        objectName: "mediaDrive"
                        width: 260
                        hint: Words.mediaMountAs
                        choices: box.media.drives.filter(one => one.enabled)
                        value: box.media.drive
                        onPicked: value => box.media.chooseDrive(value)
                    }
                }
                Setting {
                    label: Words.mediaStartFromIt

                    Choice {
                        objectName: "mediaBoot"
                        width: 260
                        hint: Words.mediaStartFromIt
                        choices: box.media.boots.filter(one => one.enabled)
                        value: box.media.boot
                        onPicked: value => box.media.chooseBoot(value)
                    }
                }
                Setting {
                    label: Words.mediaRestartItNow
                    hint: Words.mediaRestartItNowHint

                    Check {
                        objectName: "mediaRestart"
                        enabled: box.media.restartEnabled
                        checked: box.media.restart
                        Accessible.name: Words.mediaRestartItNow
                        onToggled: box.media.chooseRestart(checked)
                    }
                }
            }
        }
        // The images: all that is left of the box between the lines above and what is said under it, and it scrolls.
        Rectangle {
            id: frame

            y: how.y + how.height + 14
            width: parent.width
            height: Math.max(60, said.y - 12 - y)
            radius: Theme.radius
            color: Theme.inset
            border.width: 1
            border.color: Theme.line

            ListView {
                id: list

                objectName: "mediaList"
                x: 1
                y: 1
                width: parent.width - 2
                height: parent.height - 2
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: box.media.images
                Accessible.role: Accessible.List
                Accessible.name: Words.mediaImages

                delegate: Item {
                    id: row

                    required property var modelData
                    required property int index

                    width: list.width
                    height: 48

                    Rectangle {
                        anchors.fill: parent
                        color: row.modelData.picked ? Theme.accentSoft : "transparent"
                    }
                    Rectangle {
                        width: parent.width
                        height: 1
                        visible: row.index > 0
                        color: Theme.line
                    }
                    Rectangle {
                        id: dot

                        x: 14
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        radius: 8
                        color: Theme.inset
                        border.width: 1
                        border.color: row.modelData.picked ? Theme.accentLine : Theme.lineStrong

                        Rectangle {
                            anchors.centerIn: parent
                            width: 8
                            height: 8
                            radius: 4
                            visible: row.modelData.picked
                            color: Theme.accent
                        }
                    }
                    Column {
                        x: dot.x + dot.width + 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - x - 14
                        spacing: 2

                        Text {
                            textFormat: Text.PlainText
                            objectName: "mediaImage"
                            width: parent.width
                            text: row.modelData.name
                            color: Theme.text
                            font.pixelSize: Theme.textSize
                            elide: Text.ElideMiddle
                        }
                        Text {
                            textFormat: Text.PlainText
                            width: parent.width
                            visible: text !== ""
                            text: row.modelData.hint
                            color: Theme.muted
                            font.pixelSize: Theme.smallSize
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        objectName: "mediaPick"
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: row.modelData.name
                        Accessible.checked: row.modelData.picked
                        onClicked: box.media.pick(row.modelData.id)
                    }
                }
            }
            Text {
                textFormat: Text.PlainText
                objectName: "mediaNothing"
                anchors.centerIn: parent
                width: parent.width - 36
                visible: list.count === 0
                text: box.media.nothing
                color: Theme.muted
                font.pixelSize: Theme.textSize
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
            ScrollMark {
                flickable: list
            }
        }
        // What is said: one line, two at the most, in its place; while something is out, it spins and counts.
        Item {
            id: said

            y: foot.y - 10 - height
            width: parent.width
            height: 36

            Text {
                textFormat: Text.PlainText
                objectName: "mediaSaid"
                width: parent.width - 84
                anchors.verticalCenter: parent.verticalCenter
                text: box.media.said
                color: box.media.saidBad ? Theme.bad : Theme.text
                font.pixelSize: Theme.textSize
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            // While something is out: that it is, and for how long, in a place of their own at its right.
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    text: box.media.waited
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                }
                Spinner {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: box.media.working
                }
            }
        }
        Text {
            id: foot

            textFormat: Text.PlainText
            objectName: "mediaFoot"
            y: parent.height - height
            width: parent.width
            height: 4 * Math.ceil(fontMetrics.height)
            text: box.media.foot
            color: Theme.muted
            font.pixelSize: Theme.smallSize
            wrapMode: Text.WordWrap
            maximumLineCount: 4
            elide: Text.ElideRight

            FontMetrics {
                id: fontMetrics

                font: foot.font
            }
        }
    }
    // The question before something is done: over the box, naming the machine and the image.
    Dialog {
        id: question

        objectName: "mediaQuestion"
        readonly property bool there: box.asked.title !== undefined

        title: box.asked.title ?? ""
        width: Math.min(460, box.width - 24)
        onThereChanged: {
            if (there)
                open();
            else
                close();
        }
        onOpened: no.forceActiveFocus()
        onClosed: {
            if (there)
                box.media.answer(false);
        }

        Column {
            width: question.availableWidth
            spacing: 14

            Text {
                textFormat: Text.PlainText
                objectName: "mediaQuestionText"
                width: parent.width
                text: box.asked.text ?? ""
                color: Theme.text
                font.pixelSize: Theme.textSize
                wrapMode: Text.WordWrap
            }
            Item {
                width: parent.width
                height: Theme.controlHeight

                Row {
                    anchors.right: parent.right
                    spacing: 8

                    Button {
                        id: no

                        objectName: "mediaNo"
                        text: Words.cancel
                        onClicked: box.media.answer(false)
                    }
                    Button {
                        objectName: "mediaYes"
                        primary: box.asked.danger === false
                        danger: box.asked.danger === true
                        text: box.asked.yes ?? ""
                        onClicked: box.media.answer(true)
                    }
                }
            }
        }
    }
}
