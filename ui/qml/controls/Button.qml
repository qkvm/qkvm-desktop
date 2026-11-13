import QtQuick
import QtQuick.Templates as T
import Qkvm

// The one button: the page's shape and colours. `primary` is the filled blue one, `danger` the
// filled red one that says yes to what cannot be taken back (the page's "danger fill"), `risky` the
// one with its words and its frame in red that leads to such a yes, which asks first (the page's
// "danger"), `ghost` has no frame until the pointer is over it, `noted` has its symbol and its words
// in the colour of a warning: what it stands for is not as it usually is. `symbol` puts one of the
// page's icons before the words, and a button with a symbol and no words is as wide as it is high;
// `drops` puts the small arrow of a button that opens something behind them. `hint` says what the
// button is for to whoever cannot see it.
T.Button {
    id: control

    property bool primary: false
    property bool danger: false
    property bool risky: false
    property bool ghost: false
    property bool noted: false
    property string symbol
    property bool drops: false
    property string hint

    readonly property color ink: primary || danger ? Theme.textOnAccent : risky ? Theme.bad : noted ? Theme.warn : Theme.text

    implicitWidth: Math.max(Theme.controlHeight, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Theme.controlHeight
    leftPadding: text === "" ? 0 : 12
    rightPadding: text === "" ? 0 : 12
    spacing: 6
    // The program's own letters: a control would otherwise take those of Windows' theme.
    font.family: Application.font.family
    font.pixelSize: Theme.textSize
    font.weight: Font.Medium
    opacity: enabled ? 1 : 0.5

    Accessible.name: text !== "" ? text : hint
    Accessible.description: text !== "" ? hint : ""

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight

        Row {
            id: row

            anchors.centerIn: parent
            spacing: control.spacing

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.symbol !== ""
                name: control.symbol
                color: control.ink
            }
            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                visible: control.text !== ""
                text: control.text
                font: control.font
                color: control.ink
            }
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.drops
                name: "chevronDown"
                size: 12
                color: Theme.muted
            }
        }
    }

    background: Rectangle {
        radius: Theme.radius
        color: {
            if (control.danger)
                return control.down || control.hovered ? Qt.darker(Theme.bad, 1.15) : Theme.bad;
            if (control.primary)
                return control.down ? Theme.accentActive : control.hovered ? Theme.accentHover : Theme.accent;
            if (control.risky && (control.hovered || control.down))
                return Theme.badSoft;
            if (control.hovered || control.down || control.checked)
                return Theme.hover;
            return control.ghost ? "transparent" : Theme.panel;
        }
        border.width: 1
        border.color: control.danger ? Theme.bad : control.risky ? Qt.rgba(Theme.bad.r, Theme.bad.g, Theme.bad.b, 0.55) : control.primary ? Theme.accentLine : control.ghost ? "transparent" : Theme.lineStrong

        Behavior on color {
            ColorAnimation {
                duration: Theme.fast
            }
        }

        // Where the keyboard is: the page's outline, two pixels out.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: Theme.radius + 4
            color: "transparent"
            border.width: 2
            border.color: Theme.accentText
            visible: control.visualFocus
        }
    }
}
