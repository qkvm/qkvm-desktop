pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// Discovery only supplies an address. Signing in and deciding what certificate to remember remain
// the existing Add KVM workflow's job.
Dialog {
    id: box

    required property DiscoveryView discovery

    title: Words.discoverKvms
    width: Math.min(640, parent ? parent.width - 24 : 640)
    onAboutToShow: discovery.prepare()
    onAboutToHide: discovery.stop()
    onClosed: discovery.closed()

    Column {
        width: box.availableWidth
        spacing: 14

        Flickable {
            id: scrolled

            objectName: "discoveryContents"
            width: parent.width
            height: Math.min(contents.height, Math.max(100, (box.parent ? box.parent.height : 800) - 56 - 36 - 24 - 28 - status.height - Theme.controlHeight))
            contentHeight: contents.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: contents

                width: scrolled.width
                spacing: 12

                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: Words.discoveryHint
                    color: Theme.muted
                    font.pixelSize: Theme.textSize
                    wrapMode: Text.WordWrap
                }
                Repeater {
                    model: box.discovery.networks

                    delegate: Check {
                        id: network

                        required property var modelData
                        required property int index

                        objectName: "discoveryNetwork"
                        width: contents.width
                        enabled: !box.discovery.busy
                        checked: modelData.selected
                        text: Words.discoveryNetwork.arg(modelData.label).arg(modelData.cidr).arg(modelData.hostCount)
                        onToggled: box.discovery.selectNetwork(index, checked)
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    visible: box.discovery.networks.length === 0
                    text: Words.discoveryNoNetworks
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.WordWrap
                }
                Field {
                    id: scope

                    objectName: "discoveryScope"
                    width: parent.width
                    enabled: !box.discovery.busy
                    label: Words.discoveryScope
                    placeholder: "192.168.1.0/24"
                    text: box.discovery.manualScope
                    onEdited: box.discovery.manualScope = text
                    onAccepted: box.discovery.start(false)
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: Words.discoveryScopeHint
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.WordWrap
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "discoveryProblem"
                    width: parent.width
                    visible: text !== ""
                    text: box.discovery.problem
                    color: Theme.bad
                    font.pixelSize: Theme.textSize
                    wrapMode: Text.WordWrap
                }
                Text {
                    textFormat: Text.PlainText
                    objectName: "discoveryConfirmation"
                    width: parent.width
                    visible: box.discovery.confirmationNeeded
                    text: Words.discoveryConfirm.arg(box.discovery.planned)
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    wrapMode: Text.WordWrap
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    visible: box.discovery.count > 0
                    text: Words.discoveryFound.arg(box.discovery.count)
                    color: Theme.text
                    font.pixelSize: Theme.headingSize
                    font.weight: Font.DemiBold
                }
                ListView {
                    id: results

                    objectName: "discoveryResults"
                    width: parent.width
                    height: Math.min(contentHeight, 220)
                    visible: box.discovery.count > 0
                    model: box.discovery
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    spacing: 8

                    delegate: Rectangle {
                        id: result

                        required property int index
                        required property string address
                        required property string family
                        required property bool supported
                        required property bool encrypted

                        width: results.width
                        height: 76
                        radius: Theme.radius
                        color: Theme.inset
                        border.color: Theme.line

                        Column {
                            x: 10
                            y: 10
                            width: parent.width - choose.width - 30
                            spacing: 4

                            Text {
                                textFormat: Text.PlainText
                                objectName: "discoveryAddress"
                                width: parent.width
                                text: result.address
                                color: Theme.text
                                font.pixelSize: Theme.textSize
                                elide: Text.ElideRight
                            }
                            Text {
                                textFormat: Text.PlainText
                                width: parent.width
                                text: result.family === "kvmd" ? Words.discoveryKvmd : result.family === "jetkvm" ? Words.discoveryJetKvm : Words.discoveryRedfish
                                color: Theme.muted
                                font.pixelSize: Theme.smallSize
                                elide: Text.ElideRight
                            }
                            Text {
                                textFormat: Text.PlainText
                                width: parent.width
                                text: result.supported ? (result.encrypted ? Words.discoveryCheckAgain : Words.discoveryPlainHttp) : Words.discoveryServerNeeded
                                color: Theme.muted
                                font.pixelSize: Theme.smallSize
                                elide: Text.ElideRight
                            }
                        }
                        Button {
                            id: choose

                            objectName: "discoveryChoose"
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            visible: result.supported
                            enabled: box.discovery.active
                            text: Words.discoveryUseAddress
                            onClicked: box.discovery.choose(result.index)
                        }
                    }
                }
            }
        }
        Text {
            id: status

            textFormat: Text.PlainText
            objectName: "discoveryStatus"
            width: parent.width
            text: box.discovery.busy ? Words.discoveryProgress.arg(box.discovery.completed).arg(box.discovery.total).arg(box.discovery.count) : box.discovery.state === "complete" ? Words.discoveryComplete.arg(box.discovery.count) : box.discovery.state === "failed" ? Words.discoveryFailed : box.discovery.state === "stopped" ? Words.discoveryStopped.arg(box.discovery.completed).arg(box.discovery.total) : Words.discoveryReady
            color: Theme.muted
            font.pixelSize: Theme.smallSize
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
        Row {
            anchors.right: parent.right
            spacing: 8

            Button {
                objectName: "discoveryClose"
                text: Words.close
                onClicked: box.close()
            }
            Button {
                objectName: "discoveryStop"
                visible: box.discovery.busy
                text: Words.stop
                onClicked: box.discovery.stop()
            }
            Button {
                objectName: "discoveryStart"
                visible: !box.discovery.busy
                primary: true
                enabled: box.discovery.active
                text: box.discovery.confirmationNeeded ? Words.discoverySearchConfirmed.arg(box.discovery.planned) : Words.discoverySearch
                onClicked: box.discovery.start(box.discovery.confirmationNeeded)
            }
        }
    }
}
