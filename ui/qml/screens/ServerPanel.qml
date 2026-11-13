pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import Qkvm

// A server seen through its controller gives no picture: where a KVM's card shows the screen, a
// server's shows how it is doing. As on the page, everything in it is sized by the card, so that it
// reads on a wall of many: `unit` is a hundredth of the card's width.
Rectangle {
    id: panel

    objectName: "serverPanel"
    // {power, tone, locating, hardware, why, setupLeft, readings: [{label, value, tone}]}
    required property var server

    readonly property real unit: width / 100
    readonly property bool isOn: server.power === "on"
    readonly property bool small: width <= 250
    // The sizes of the letters, by themselves: a font's spacing cannot be bound to that font's own size.
    readonly property real powerSize: between(18, 11 * unit, 72)
    readonly property real tagSize: between(10, 2.6 * unit, 14)
    readonly property real labelSize: between(9, 2.5 * unit, 13)
    readonly property color glow: !isOn ? "transparent" : server.tone === "bad" ? Qt.rgba(239 / 255, 68 / 255, 68 / 255, 0.18) : server.tone === "warn" ? Qt.rgba(245 / 255, 158 / 255, 11 / 255, 0.13) : Qt.rgba(34 / 255, 197 / 255, 94 / 255, 0.13)

    function between(least: real, wanted: real, most: real): real {
        return Math.max(least, Math.min(wanted, most));
    }
    function toneColor(tone: string): color {
        return tone === "ok" ? Theme.screenOk : tone === "warn" ? Theme.screenWarn : tone === "bad" ? Theme.screenBad : Theme.screenText;
    }

    color: Theme.screen
    clip: true

    // The faint lines of a readout. (None while it is still being laid out, as high as nothing,
    // or less: a machine shown large makes its screen before its bars are placed.)
    Repeater {
        model: Math.max(0, Math.ceil(panel.height / lines.step))

        delegate: Rectangle {
            required property int index

            y: index * lines.step
            width: panel.width
            height: 1
            color: Qt.rgba(148 / 255, 163 / 255, 184 / 255, 0.05)
        }
    }
    QtObject {
        id: lines

        readonly property real step: Math.round(panel.between(10, 4 * panel.unit, 26))
    }

    // A glow from the top left corner in the colour of its health. A server that is off has none.
    Shape {
        visible: panel.isOn
        width: panel.width * 1.8
        height: panel.width * 1.8
        x: -width / 2
        y: -height / 2
        transform: Scale {
            origin.x: panel.width * 0.9
            origin.y: panel.width * 0.9
            yScale: panel.height * 1.2 / (panel.width * 0.9)
        }

        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: panel.width * 0.9
                centerY: panel.width * 0.9
                focalX: centerX
                focalY: centerY
                centerRadius: panel.width * 0.9

                GradientStop {
                    position: 0
                    color: panel.glow
                }
                GradientStop {
                    position: 0.62
                    color: Qt.rgba(panel.glow.r, panel.glow.g, panel.glow.b, 0)
                }
            }
            startX: 0
            startY: 0

            PathLine {
                x: panel.width * 1.8
                y: 0
            }
            PathLine {
                x: panel.width * 1.8
                y: panel.width * 1.8
            }
            PathLine {
                x: 0
                y: panel.width * 1.8
            }
            PathLine {
                x: 0
                y: 0
            }
        }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: panel.between(10, 6 * panel.unit, 56)
        anchors.rightMargin: anchors.leftMargin
        spacing: panel.between(6, 3.2 * panel.unit, 22)

        // On or Off, large, with its light; and at the right what kind of machine this is.
        Item {
            width: parent.width
            height: power.implicitHeight

            // The light, with a faint ring around it.
            Rectangle {
                id: light

                anchors.verticalCenter: parent.verticalCenter
                width: panel.between(8, 3 * panel.unit, 18) + 6
                height: width
                radius: width / 2
                color: panel.isOn ? Qt.rgba(34 / 255, 197 / 255, 94 / 255, 0.18) : Qt.rgba(71 / 255, 85 / 255, 105 / 255, 0.25)

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width - 6
                    height: width
                    radius: width / 2
                    color: panel.isOn ? "#22c55e" : "#475569"
                }
            }
            Text {
                id: power

                textFormat: Text.PlainText
                anchors.left: light.right
                anchors.leftMargin: panel.between(6, 2.4 * panel.unit, 16)
                text: panel.isOn ? Words.powerOn : Words.powerOff
                color: panel.isOn ? Theme.screenText : Theme.screenMuted
                font.pixelSize: panel.powerSize
                font.weight: Font.DemiBold
                font.letterSpacing: -0.02 * panel.powerSize
            }
            Row {
                anchors.right: parent.right
                anchors.baseline: power.baseline
                visible: !panel.small || panel.server.locating
                spacing: 6

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: panel.server.locating
                    width: panel.small ? 9 : panel.tagSize * 0.75
                    height: width
                    radius: width / 2
                    color: "#3b82f6"
                }
                Text {
                    id: tag

                    textFormat: Text.PlainText
                    visible: !panel.small
                    text: panel.server.locating ? Words.locatorLightOn : Words.server
                    color: panel.server.locating ? "#93c5fd" : Theme.screenFaint
                    font.pixelSize: panel.tagSize
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.12 * panel.tagSize
                }
            }
        }

        // Health, temperature, fans: on a small card only the first two.
        Row {
            spacing: panel.between(8, 4 * panel.unit, 36)

            Repeater {
                model: panel.server.readings

                delegate: Column {
                    id: reading

                    required property var modelData
                    required property int index

                    visible: !panel.small || index < 2
                    spacing: 2

                    Text {
                        textFormat: Text.PlainText
                        text: reading.modelData.label
                        color: reading.modelData.tone === "warn" ? Theme.screenWarn : Theme.screenMuted
                        font.pixelSize: panel.labelSize
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.08 * panel.labelSize
                    }
                    Text {
                        textFormat: Text.PlainText
                        text: reading.modelData.value
                        color: panel.toneColor(reading.modelData.tone)
                        font.pixelSize: panel.between(12, 4.4 * panel.unit, 26)
                        font.weight: Font.DemiBold
                    }
                }
            }
        }

        Text {
            textFormat: Text.PlainText
            width: parent.width
            visible: !panel.small
            text: panel.server.hardware
            color: Theme.screenMuted
            font.pixelSize: panel.between(10, 2.9 * panel.unit, 15)
            elide: Text.ElideRight
        }
        // Its BIOS setup boot override may be set, left by a start into BIOS setup that did not end:
        // said first, until a look shows it off.
        Text {
            objectName: "serverPanelSetupLeft"
            textFormat: Text.PlainText
            width: parent.width
            visible: text !== ""
            text: panel.server.setupLeft ?? ""
            color: Theme.screenWarn
            font.pixelSize: panel.between(10, 2.7 * panel.unit, 14)
            font.weight: Font.DemiBold
            lineHeight: 1.35
            wrapMode: Text.WordWrap
            maximumLineCount: panel.small ? 2 : 3
            elide: Text.ElideRight
        }
        // Why there is no picture where there could be one (a PC with Intel AMT in health only): on
        // a card in the lines it has room for, as the page's panel says it.
        Text {
            objectName: "serverPanelWhy"
            textFormat: Text.PlainText
            width: parent.width
            visible: text !== ""
            text: panel.server.why ?? ""
            color: Theme.screenText
            font.pixelSize: panel.between(10, 2.7 * panel.unit, 14)
            lineHeight: 1.35
            wrapMode: Text.WordWrap
            maximumLineCount: panel.small ? 2 : 3
            elide: Text.ElideRight
        }
    }
}
