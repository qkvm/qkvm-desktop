pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import Qkvm

// A machine's screen, wherever it is shown: on its card on the wall, and large. Its picture; or, for
// a machine without a live one, why; or, for a server whose card says how it is doing, that. Sized
// by whoever places it.
//
// The picture itself is the engine's, drawn underneath everything here: nothing opaque may lie over
// the VideoTile but what is meant to cover the picture.
Item {
    id: screen

    required property int stream
    required property string status
    required property string title
    required property string text
    required property string action
    required property var ports
    required property var server
    // What is shown is as it was a while ago, and may be so no longer: the server that tells of
    // the machine does not answer. A picture that is as bright as a live one would be taken for one.
    property bool old: false
    // The view it is shown in where it may be larger than that (the enlarged view at a scale): of its
    // picture only what lies inside that item is drawn.
    property Item clipTo: null

    // A server whose card says how it is doing. One whose own screen is shown is shown as a KVM
    // is, and has no such panel.
    readonly property bool isServer: server.power !== undefined
    readonly property bool small: width <= 250

    signal actionAsked
    // Another of the computers behind a KVM was chosen, by its number counted from 0.
    signal portAsked(int port)

    VideoTile {
        anchors.fill: parent
        visible: screen.stream !== 0
        stream: screen.stream
        clipTo: screen.clipTo
    }
    // A machine that has no picture at all still has a screen's black behind what it says.
    Rectangle {
        anchors.fill: parent
        visible: screen.stream === 0 && !screen.isServer
        color: Theme.screen
    }
    // Made only for a server: a wall of KVMs has none of these.
    Loader {
        anchors.fill: parent
        active: screen.isServer

        sourceComponent: ServerPanel {
            server: screen.server
        }
    }
    // Over the picture, and under what the card says in words.
    Rectangle {
        objectName: "oldVeil"
        anchors.fill: parent
        visible: screen.old
        color: Theme.screen
        opacity: 0.6
    }

    // What a machine says when its picture is not live: an icon, a heading, a sentence, and
    // sometimes a way out. A small one keeps the heading alone.
    Rectangle {
        objectName: "veil"
        anchors.fill: parent
        // Only where there is something to say: a server that runs with a fault keeps its screen
        // in sight, and the card's edge says the rest.
        visible: screen.title !== "" && !screen.isServer
        color: Theme.screenVeil

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 28, 290)
            spacing: 4

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: !screen.small
                width: 38
                height: 38
                radius: Theme.radius
                color: Qt.rgba(30 / 255, 41 / 255, 59 / 255, 0.8)
                border.width: 1
                border.color: Qt.rgba(203 / 255, 213 / 255, 225 / 255, 0.2)

                Icon {
                    objectName: "veilSign"
                    anchors.centerIn: parent
                    visible: screen.status !== "connecting"
                    size: 18
                    name: screen.status === "no_signal" ? "system" : screen.status === "offline" ? "wifiOff" : screen.status === "auth" ? "lock" : screen.status === "busy" ? "eye" : "alert"
                    color: screen.status === "no_signal" || screen.status === "busy" ? "#f59e0b" : Theme.screenBad
                }
                // A machine that is on its way (connecting, restarting) is waited for, as on the
                // page: no sign of trouble.
                Spinner {
                    objectName: "veilSpinner"
                    anchors.centerIn: parent
                    visible: screen.status === "connecting"
                    size: 18
                    color: Theme.screenMuted
                }
            }
            Item {
                visible: !screen.small
                width: 1
                height: 4
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: screen.title
                color: Theme.screenText
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                visible: !screen.small && screen.text !== ""
                horizontalAlignment: Text.AlignHCenter
                text: screen.text
                color: Theme.screenMuted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.WordWrap
            }
            Item {
                visible: veilButton.visible
                width: 1
                height: 8
            }
            T.Button {
                id: veilButton

                objectName: "cardAction"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: !screen.small && screen.action !== ""
                implicitWidth: veilWords.implicitWidth + 24
                implicitHeight: 28
                text: screen.action
                onClicked: screen.actionAsked()

                contentItem: Text {
                    id: veilWords

                    textFormat: Text.PlainText
                    text: veilButton.text
                    color: Theme.screenText
                    font.pixelSize: Theme.textSize
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: Theme.radius
                    color: veilButton.hovered ? "#334155" : "#1e293b"
                    border.width: 1
                    border.color: Theme.screenLine
                }
            }
        }
    }

    // A KVM with several computers behind it: one number for each, and the one whose picture
    // this is stands out. Always there, because it says whose picture it is.
    Rectangle {
        x: 8
        y: parent.height - height - 8
        visible: screen.ports.length > 0
        width: portRow.implicitWidth + 6
        height: screen.small ? 24 : 28
        radius: Theme.radius
        color: Theme.screenPanel
        border.width: 1
        border.color: Theme.screenLine

        Row {
            id: portRow

            anchors.centerIn: parent
            spacing: 2

            Repeater {
                model: screen.ports

                delegate: T.Button {
                    id: port

                    required property var modelData
                    required property int index

                    objectName: "port"
                    implicitWidth: screen.small ? 20 : 24
                    implicitHeight: screen.small ? 18 : 22
                    opacity: modelData.empty && !modelData.shown ? 0.45 : 1
                    Accessible.name: modelData.hint
                    onClicked: screen.portAsked(index)

                    contentItem: Text {
                        textFormat: Text.PlainText
                        text: port.modelData.label
                        color: port.modelData.shown || port.hovered ? "#ffffff" : "#cbd5e1"
                        font.pixelSize: screen.small ? 11 : 12
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 3
                        color: port.modelData.shown ? "#1d4ed8" : "transparent"
                    }
                }
            }
        }
    }
}
