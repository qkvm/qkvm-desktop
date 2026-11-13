pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// Keys of one's own choosing for a machine, as the page's box for them has it: the keys that are
// held, each a chip to turn on; one key more, from a list; and beside them what that makes. Whether
// what was chosen can be sent is the program's to say: it sends it on its way, or says here what is
// amiss with it.
Dialog {
    id: box

    required property ActionsView actions

    readonly property var form: actions.form
    readonly property bool there: form.kind === "keys"
    // Which of the held keys are on, by the server's word for each; and the one key more.
    property var held: ({})
    property string other: ""
    // In the order the server presses them: the held ones first.
    readonly property string chosen: actions.heldKeys.filter(key => box.held[key.value] === true).map(key => key.value).concat(other === "" ? [] : [other]).join(",")

    title: Words.sendKeys
    width: Math.min(520, parent ? parent.width - 24 : 520)

    onThereChanged: {
        if (there)
            open();
        else
            close();
    }
    // Each time anew: what was chosen for another machine is not this one's.
    onAboutToShow: {
        held = ({});
        other = "";
    }
    onClosed: {
        if (there)
            actions.closeForm();
    }

    Column {
        width: box.availableWidth
        spacing: 14

        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: Words.pressedTogether
            color: Theme.muted
            font.pixelSize: Theme.textSize
            wrapMode: Text.WordWrap
        }
        Flow {
            width: parent.width
            spacing: 8

            Repeater {
                model: box.actions.heldKeys

                delegate: Chip {
                    id: chip

                    required property var modelData

                    objectName: "held:" + modelData.value
                    text: modelData.label
                    on: box.held[modelData.value] === true
                    onClicked: {
                        const next = Object.assign({}, box.held);
                        next[chip.modelData.value] = !chip.on;
                        box.held = next;
                    }
                }
            }
            Choice {
                objectName: "otherKey"
                hint: Words.key
                choices: box.actions.otherKeys
                value: box.other
                onPicked: value => box.other = value
            }
            // What that makes, as a person reads keys.
            Rectangle {
                width: made.implicitWidth + 16
                height: Theme.controlHeight
                radius: 3
                color: Theme.inset
                border.width: 1
                border.color: Theme.line

                Text {
                    id: made

                    textFormat: Text.PlainText
                    objectName: "keysChosen"
                    anchors.centerIn: parent
                    text: box.chosen === "" ? Words.nothingChosen : box.actions.read(box.chosen)
                    color: box.chosen === "" ? Theme.muted : Theme.text
                    font.pixelSize: Theme.smallSize
                }
            }
        }
        Text {
            textFormat: Text.PlainText
            objectName: "keysProblem"
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
                    objectName: "keysCancel"
                    text: Words.cancel
                    onClicked: box.actions.closeForm()
                }
                Button {
                    objectName: "keysContinue"
                    primary: true
                    text: Words.carryOn
                    onClicked: box.actions.sendKeys(box.chosen)
                }
            }
        }
    }
}
