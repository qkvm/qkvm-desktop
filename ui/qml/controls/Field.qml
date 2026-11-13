import QtQuick
import QtQuick.Templates as T
import Qkvm

// A line to type into, with what it is for written above it, as the page's forms have it. `secret`
// hides what is typed, and gives the line an eye, as the page's has one: pressed, what was typed is
// shown; pressed again, it is hidden. It is hidden again by itself when it is sent (`conceal()`,
// which whoever sends it calls) and when the line goes out of sight: a password does not stay on
// show for whoever walks past the screen.
Column {
    id: field

    property string label
    property alias text: input.text
    property alias placeholder: input.placeholderText
    // What may be typed (none: anything).
    property alias validator: input.validator
    property bool secret: false
    // What was typed into a secret line is on show (its eye was pressed).
    property bool shown: false
    // The keyboard is in this line.
    readonly property bool hasKeyboard: input.activeFocus

    signal accepted
    // Somebody typed in it: not said where the text was put there by the program.
    signal edited

    function takeFocus() {
        input.forceActiveFocus();
    }
    function conceal() {
        shown = false;
    }

    spacing: 6
    onVisibleChanged: {
        if (!visible)
            conceal();
    }
    onSecretChanged: conceal()

    // As the page writes it (`.field > span`): as it is spelled, not in capitals, which would
    // turn qKVM into QKVM.
    Text {
        textFormat: Text.PlainText
        text: field.label
        color: Theme.text
        font.pixelSize: Theme.textSize
        font.weight: Font.Medium
    }
    T.TextField {
        id: input

        width: field.width
        implicitHeight: 34
        objectName: "line"
        leftPadding: 10
        rightPadding: field.secret ? eye.width + 4 : 10
        verticalAlignment: TextInput.AlignVCenter
        color: Theme.text
        placeholderTextColor: Theme.muted
        selectionColor: Theme.accent
        selectedTextColor: Theme.textOnAccent
        font.family: Application.font.family
        font.pixelSize: Theme.textSize
        selectByMouse: true
        echoMode: field.secret && !field.shown ? TextInput.Password : TextInput.Normal
        // (On show or not, what is typed into a secret line is nobody's to learn or to suggest.)
        inputMethodHints: field.secret ? Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase : Qt.ImhNone
        Accessible.name: field.label
        onAccepted: field.accepted()
        onTextEdited: field.edited()

        Text {
            textFormat: Text.PlainText
            x: input.leftPadding
            anchors.verticalCenter: parent.verticalCenter
            visible: input.text === "" && input.preeditText === ""
            text: input.placeholderText
            color: input.placeholderTextColor
            font: input.font
        }
        // The eye. Not a stop for the Tab key: the lines of a form follow one another as before.
        T.AbstractButton {
            id: eye

            objectName: "eye"
            visible: field.secret
            anchors.right: parent.right
            anchors.rightMargin: 1
            anchors.verticalCenter: parent.verticalCenter
            width: 34
            height: parent.height - 2
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            Accessible.role: Accessible.Button
            Accessible.name: field.shown ? Words.hidePassword : Words.showPassword
            onClicked: {
                field.shown = !field.shown;
                input.forceActiveFocus();
            }

            contentItem: Item {
                // (As a picture of itself, a layer: a line to type into may stand in what scrolls,
                // which cuts off what is scrolled out over its edge, and a drawing of lines is not
                // held to that edge where Qt Quick draws in software. The eye of a line that was cut
                // off stood over the buttons under it: Check.qml says the same of its mark.)
                Icon {
                    anchors.centerIn: parent
                    name: field.shown ? "eyeOff" : "eye"
                    color: eye.hovered ? Theme.text : Theme.muted
                    size: 16
                    layer.enabled: true
                }
            }
        }

        background: Rectangle {
            radius: Theme.radius
            color: Theme.inset
            border.width: 1
            border.color: input.activeFocus ? Theme.accentText : Theme.lineStrong

            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: Theme.radius + 3
                color: "transparent"
                border.width: 3
                border.color: Theme.accentSoft
                visible: input.activeFocus
            }
        }
    }
}
