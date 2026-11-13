pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// Asked before something is done that cannot be taken back, or that does harm where it lands by a
// slip: what, to which machine, and what is to be known before saying yes. As the page asks it.
// Esc, the cross and a click beside it are a no; the keyboard starts at Cancel.
//
// About several machines it names them, each with how it is; asks for a tick as well where what is
// done can lose somebody's work, and the yes is not to be had before; and asks how to go about it:
// a few at a time, or one after the other with a pause between them.
Dialog {
    id: box

    required property ActionsView actions

    readonly property var asked: actions.question
    readonly property bool there: asked.title !== undefined
    readonly property var targets: asked.targets ?? []
    readonly property string toTick: asked.tick ?? ""
    readonly property var paces: asked.paces ?? []
    // How to go about it: the seconds between one machine and the next, or -1 for a few at a time.
    property int pace: -1
    // As high as the window lets the box be, and what the box needs beside the list of the machines:
    // the list takes what is left of the one after the other, and scrolls.
    readonly property real room: (parent ? parent.height : 600) - 24
    readonly property real beside: implicitHeaderHeight + topPadding + bottomPadding + said.height + buttons.height + 2 * 14 + (detail.text !== "" ? detail.height + 14 : 0) + (warning.text !== "" ? warning.height + 14 : 0) + (toTick !== "" ? ticked.height + 14 : 0) + (paces.length > 0 ? paceRow.height + 14 : 0)

    title: asked.title ?? ""
    width: Math.min(460, parent ? parent.width - 24 : 460)

    onThereChanged: {
        if (there)
            open();
        else
            close();
    }
    // Each question is answered for itself: neither the tick nor the pace of the last one holds.
    onAboutToShow: {
        ticked.checked = false;
        pace = -1;
    }
    onOpened: no.forceActiveFocus()
    onClosed: {
        if (there)
            actions.answer(false);
    }

    Column {
        width: box.availableWidth
        spacing: 14

        Text {
            id: said

            textFormat: Text.PlainText
            objectName: "questionText"
            width: parent.width
            text: box.asked.text ?? ""
            color: Theme.text
            font.pixelSize: Theme.textSize
            wrapMode: Text.WordWrap
        }
        // The machines it is about, where they are several: the list scrolls where they are many.
        Rectangle {
            objectName: "questionTargets"
            width: parent.width
            height: Math.min(listed.contentHeight, 200, Math.max(60, box.room - box.beside - 2)) + 2
            visible: box.targets.length > 0
            radius: Theme.radius
            color: Theme.inset
            border.width: 1
            border.color: Theme.line

            ListView {
                id: listed

                x: 1
                y: 1
                width: parent.width - 2
                height: parent.height - 2
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: box.targets

                delegate: Item {
                    id: target

                    required property var modelData
                    required property int index

                    objectName: "questionTarget"
                    width: listed.width
                    height: 30

                    Rectangle {
                        visible: target.index > 0
                        width: parent.width
                        height: 1
                        color: Theme.line
                    }
                    Rectangle {
                        id: dot

                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8
                        height: 8
                        radius: 4
                        color: Theme.stateColor(target.modelData.state)
                    }
                    Text {
                        id: targetName

                        textFormat: Text.PlainText
                        anchors.left: dot.right
                        anchors.leftMargin: 9
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width * 0.4)
                        text: target.modelData.name
                        color: Theme.text
                        font.pixelSize: Theme.textSize
                        elide: Text.ElideRight
                    }
                    Text {
                        textFormat: Text.PlainText
                        anchors.left: targetName.right
                        anchors.leftMargin: 9
                        anchors.right: how.left
                        anchors.rightMargin: 9
                        anchors.verticalCenter: parent.verticalCenter
                        text: target.modelData.host
                        color: Theme.muted
                        font.pixelSize: Theme.smallSize
                        elide: Text.ElideRight
                    }
                    Text {
                        id: how

                        textFormat: Text.PlainText
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: target.modelData.label
                        color: Theme.muted
                        font.pixelSize: Theme.smallSize
                    }
                }
            }
        }
        // What would be typed, to be looked at once more.
        Note {
            id: detail

            objectName: "questionDetail"
            width: parent.width
            visible: text !== ""
            code: true
            text: box.asked.detail ?? ""
        }
        Note {
            id: warning

            objectName: "questionWarning"
            width: parent.width
            visible: text !== ""
            tone: "warn"
            text: box.asked.warning ?? ""
        }
        Check {
            id: ticked

            objectName: "questionTick"
            width: parent.width
            visible: box.toTick !== ""
            text: box.toTick
        }
        Row {
            id: paceRow

            visible: box.paces.length > 0
            spacing: 10

            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                text: Words.inTheOrderShown
                color: Theme.muted
                font.pixelSize: Theme.textSize
            }
            Choice {
                objectName: "questionPace"
                hint: Words.howFast
                choices: box.paces
                value: box.pace
                onPicked: value => box.pace = value
            }
        }
        Item {
            id: buttons

            width: parent.width
            height: Theme.controlHeight

            Row {
                anchors.right: parent.right
                spacing: 8

                Button {
                    id: no

                    objectName: "questionNo"
                    text: Words.cancel
                    onClicked: box.actions.answer(false)
                }
                // Made for the question that is asked: a button that turned from blue to red as
                // the box opened would be seen to do it.
                Loader {
                    sourceComponent: box.asked.danger === true ? cannotBeTakenBack : ordinary
                }
                Component {
                    id: ordinary

                    Button {
                        objectName: "questionYes"
                        primary: true
                        // Not to be had before what is to be ticked is ticked.
                        enabled: box.toTick === "" || ticked.checked
                        text: box.asked.yes ?? box.asked.title ?? ""
                        onClicked: box.actions.answerWith(true, box.pace, ticked.checked)
                    }
                }
                Component {
                    id: cannotBeTakenBack

                    Button {
                        objectName: "questionYes"
                        danger: true
                        enabled: box.toTick === "" || ticked.checked
                        text: box.asked.yes ?? box.asked.title ?? ""
                        onClicked: box.actions.answerWith(true, box.pace, ticked.checked)
                    }
                }
            }
        }
    }
}
