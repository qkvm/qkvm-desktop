import QtQuick
import Qkvm

// A text to type on a machine, as the page's box for it has it: the text, and whether Enter is
// pressed after it. Whether it can be sent is the program's to say: it sends it on its way, to be
// asked about once more, or says here what is amiss with it.
Dialog {
    id: box

    required property ActionsView actions

    readonly property var form: actions.form
    readonly property bool there: form.kind === "text"
    // A text is typed that is not sent: an update that would close the program waits for it.
    readonly property bool unsaved: opened && words.text !== ""

    title: Words.typeText
    width: Math.min(520, parent ? parent.width - 24 : 520)

    onThereChanged: {
        if (there)
            open();
        else {
            words.text = "";
            close();
        }
    }
    onAboutToShow: enterAfter.checked = true
    onOpened: words.takeFocus()
    // What was typed is not kept: not for the next machine, and not while the box is closed.
    onClosed: {
        words.text = "";
        if (there)
            actions.closeForm();
    }

    Column {
        readonly property bool windowShown: !Window.window || (Window.window.visible && Window.window.visibility !== Window.Minimized)

        width: box.availableWidth
        spacing: 14
        onWindowShownChanged: {
            if (!windowShown && box.there)
                box.actions.closeForm();
        }

        TextBox {
            id: words

            width: parent.width
            label: Words.text
            placeholder: "ipconfig /all"
            hint: Words.typedAsOnItsKeyboard
            most: 2000
            sensitive: true
        }
        Button {
            objectName: "loadClipboard"
            text: qsTr("Paste from clipboard")
            enabled: box.actions.allowed && !box.actions.busy
            onClicked: box.actions.loadClipboard()
        }
        Check {
            id: enterAfter

            objectName: "pressEnter"
            text: Words.pressEnterAfterwards
            checked: true
        }
        Text {
            textFormat: Text.PlainText
            objectName: "textProblem"
            width: parent.width
            visible: text !== ""
            text: box.form.problem ?? ""
            color: Theme.bad
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
                    objectName: "textCancel"
                    text: Words.cancel
                    onClicked: box.actions.closeForm()
                }
                Button {
                    objectName: "textContinue"
                    primary: true
                    text: Words.carryOn
                    onClicked: box.actions.typeText(words.text, enterAfter.checked)
                }
            }
        }
    }

    Connections {
        target: box.actions
        function onClipboardLoaded(text: string) {
            if (box.opened) {
                words.text = text;
                enterAfter.checked = false;
                words.takeFocus();
            }
        }
    }
}
