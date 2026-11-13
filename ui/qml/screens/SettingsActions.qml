pragma ComponentBehavior: Bound
import QtQuick
import Qkvm

Section {
    id: section
    required property OwnActionsView actions
    property bool shown: false
    // The editor's title where it is open with changes that are not saved (an update that would close the program waits
    // for them); empty otherwise.
    readonly property string unsavedTitle: editor.opened && !editor.removing && (nameField.text !== editor.started[0] || kind.value !== editor.started[1] || keys.text !== editor.started[2] || words.text !== "" || enter.checked !== editor.started[3] || asksFirst.checked !== editor.started[4]) ? editor.title : ""
    objectName: "settingsActions"
    name: qsTr("Saved actions")
    about: qsTr("Save key combinations or text for this PC's KVMs. Actions appear in the machine menu and can also run on selected machines.")
    does: Button {
        objectName: "addSavedAction"
        text: qsTr("Add action")
        primary: true
        enabled: section.actions !== null && section.actions.allowed
        onClicked: section.edit(null, false)
    }
    function edit(entry, removing) {
        editor.actionId = entry ? entry.id : "";
        editor.revision = entry ? entry.revision : 0;
        editor.wasText = entry ? entry.types : false;
        editor.removing = removing;
        nameField.text = entry ? entry.name : "";
        kind.value = entry && entry.types ? "text" : "keys";
        keys.text = entry ? entry.keys : "";
        words.text = "";
        editor.revealsText = false;
        enter.checked = entry ? entry.enter : false;
        asksFirst.checked = entry ? entry.danger : false;
        editor.problem = "";
        editor.started = [nameField.text, kind.value, keys.text, enter.checked, asksFirst.checked];
        editor.open();
    }
    onShownChanged: {
        if (!shown)
            editor.close();
    }
    Connections {
        target: section.actions
        function onChanged() {
            if (section.actions !== null && !section.actions.allowed)
                editor.close();
        }
    }
    Listed {
        id: list
        objectName: "savedActionsList"
        title: qsTr("Actions on this PC")
        columns: [qsTr("Action")]
        lines: [
            {
                "text": 1,
                "under": [2]
            }
        ]
        doesWidth: Math.ceil(editWidth.advanceWidth + removeWidth.advanceWidth) + 2 * 24 + 8
        nothing: qsTr("No saved actions yet. Add an action to make it available to the KVMs on this PC's wall.")
        model: section.actions !== null ? section.actions.entries : []
        delegate: ListRow {
            id: actionRow
            required property var modelData
            objectName: "savedActionRow"
            of: list
            cells: [
                {
                    "text": modelData.name,
                    "under": [(modelData.types ? qsTr("Types saved text") : modelData.keysLabel) + (modelData.enter ? qsTr(" · then Enter") : "") + (modelData.danger ? qsTr(" · asks before running") : "")]
                }
            ]
            Button {
                objectName: "editSavedAction"
                text: Words.edit
                Accessible.name: Words.editNamed.arg(actionRow.modelData.name)
                enabled: section.actions !== null && section.actions.allowed
                onClicked: section.edit(actionRow.modelData, false)
            }
            Button {
                objectName: "removeSavedAction"
                text: Words.remove
                Accessible.name: Words.removeOne.arg(actionRow.modelData.name)
                risky: true
                enabled: section.actions !== null && section.actions.allowed
                onClicked: section.edit(actionRow.modelData, true)
            }
        }
    }
    TextMetrics {
        id: editWidth
        font.family: Application.font.family
        font.pixelSize: Theme.textSize
        font.weight: Font.Medium
        text: Words.edit
    }
    TextMetrics {
        id: removeWidth
        font.family: Application.font.family
        font.pixelSize: Theme.textSize
        font.weight: Font.Medium
        text: Words.remove
    }
    Dialog {
        id: editor
        objectName: "savedActionEditor"
        property string actionId
        property int revision: 0
        property bool wasText: false
        property bool removing: false
        property bool revealsText: false
        property string problem
        // What stood in it when it was opened: name, kind, keys, Enter after, asks first.
        property var started: []
        width: Math.min(540, parent ? parent.width - 24 : 540)
        title: removing ? qsTr("Remove saved action") : actionId === "" ? qsTr("Add saved action") : qsTr("Edit saved action")
        onClosed: {
            words.text = "";
            revealsText = false;
        }
        onOpened: {
            if (!removing)
                nameField.takeFocus();
        }
        Flickable {
            width: editor.availableWidth
            implicitHeight: Math.min(contentHeight, editor.parent ? editor.parent.height - 100 : 600)
            height: implicitHeight
            contentHeight: form.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: form
                width: editor.availableWidth
                spacing: 12
                Text {
                    visible: editor.removing
                    width: parent.width
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: Theme.text
                    text: qsTr("Remove “%1” and its saved text? This does not run the action.").arg(nameField.text)
                }
                Column {
                    width: parent.width
                    spacing: 12
                    visible: !editor.removing
                    Field {
                        id: nameField
                        objectName: "savedActionName"
                        width: parent.width
                        label: qsTr("Name")
                    }
                    Choice {
                        id: kind
                        objectName: "savedActionKind"
                        width: parent.width
                        choices: [
                            {
                                value: "keys",
                                label: qsTr("Press keys")
                            },
                            {
                                value: "text",
                                label: qsTr("Type text")
                            }
                        ]
                        value: "keys"
                        onPicked: value => {
                            kind.value = value;
                            words.text = "";
                            editor.revealsText = false;
                        }
                    }
                    Field {
                        id: keys
                        objectName: "savedActionKeys"
                        visible: kind.value === "keys"
                        width: parent.width
                        label: qsTr("Keys, separated by commas")
                        placeholder: "ControlLeft,AltLeft,Delete"
                    }
                    Column {
                        width: parent.width
                        spacing: 8
                        visible: kind.value === "text"
                        Text {
                            width: parent.width
                            textFormat: Text.PlainText
                            color: Theme.muted
                            wrapMode: Text.WordWrap
                            text: editor.wasText ? qsTr("Saved text is never displayed here. Leave the replacement empty to keep it.") : qsTr("Enter up to 2,000 characters. Text is saved in Windows Credential Manager.")
                        }
                        Button {
                            text: editor.revealsText ? qsTr("Hide text") : qsTr("Show text editor")
                            onClicked: editor.revealsText = !editor.revealsText
                        }
                        TextBox {
                            id: words
                            objectName: "savedActionText"
                            width: parent.width
                            visible: editor.revealsText
                            label: editor.wasText ? qsTr("Replacement text") : qsTr("Text")
                            most: 4000
                            sensitive: true
                        }
                        Check {
                            id: enter
                            text: qsTr("Press Enter afterwards")
                        }
                    }
                    Check {
                        id: asksFirst
                        text: qsTr("Ask before running")
                    }
                }
                Text {
                    width: parent.width
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: Theme.bad
                    text: editor.problem
                    visible: text !== ""
                }
                Row {
                    spacing: 8
                    Button {
                        text: qsTr("Cancel")
                        onClicked: editor.close()
                    }
                    Button {
                        objectName: "saveSavedAction"
                        primary: true
                        text: editor.removing ? qsTr("Remove") : qsTr("Save")
                        enabled: section.actions !== null && section.actions.allowed
                        onClicked: {
                            editor.problem = editor.removing ? section.actions.remove(editor.actionId, editor.revision) : section.actions.save(editor.actionId, editor.revision, nameField.text, kind.value === "text", keys.text, words.text, enter.checked, asksFirst.checked);
                            words.text = "";
                            editor.revealsText = false;
                            if (editor.problem === "")
                                editor.close();
                        }
                    }
                }
            }
        }
    }
}
