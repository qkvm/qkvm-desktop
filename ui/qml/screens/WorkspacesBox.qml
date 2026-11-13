import QtQuick
import Qkvm

Dialog {
    id: box
    required property WorkspacesView view
    property string selected: ""
    title: qsTr("Saved workspaces")
    width: Math.min(520, parent ? parent.width - 24 : 520)
    onOpened: name.takeFocus()
    onClosed: {
        name.text = "";
        selected = "";
    }
    Connections {
        target: box.view
        function onChanged() {
            if (!box.view.allowed)
                box.close();
            if (!box.view.entries.some(entry => entry.value === box.selected))
                box.selected = "";
        }
    }
    Flickable {
        objectName: "workspaceScroll"
        width: box.availableWidth
        implicitHeight: Math.min(contentHeight, box.parent ? box.parent.height - 110 : 450)
        height: implicitHeight
        contentHeight: form.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Column {
            id: form
            width: box.availableWidth
            spacing: 12
            Text {
                width: parent.width
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: Theme.muted
                text: qsTr("Save all open windows with their sources, groups, columns and positions. Opening a workspace replaces the current layout and leaves keyboard and mouse control off.")
            }
            Choice {
                objectName: "workspaceChoice"
                width: parent.width
                hint: qsTr("Workspace")
                choices: [
                    {
                        "value": "",
                        "label": qsTr("New workspace")
                    }
                ].concat(box.view !== null ? box.view.entries : [])
                value: box.selected
                onPicked: value => {
                    box.selected = value;
                    name.text = box.view.entries.find(entry => entry.value === value)?.label ?? "";
                }
            }
            Field {
                id: name
                objectName: "workspaceName"
                width: parent.width
                label: qsTr("Workspace name")
                placeholder: qsTr("For example, Office monitors")
            }
            Flow {
                width: parent.width
                spacing: 8
                Button {
                    objectName: "workspaceSave"
                    text: qsTr("Save as new")
                    enabled: box.view !== null && box.view.allowed && name.text.trim() !== ""
                    onClicked: {
                        if (box.view.save(name.text))
                            box.selected = box.view.entries[box.view.entries.length - 1].value;
                    }
                }
                Button {
                    objectName: "workspaceUpdate"
                    text: qsTr("Update layout")
                    enabled: box.selected !== ""
                    onClicked: box.view.save(name.text, box.selected)
                }
                Button {
                    objectName: "workspaceRename"
                    text: qsTr("Rename")
                    enabled: box.selected !== ""
                    onClicked: box.view.rename(box.selected, name.text)
                }
                Button {
                    objectName: "workspaceDelete"
                    text: qsTr("Delete")
                    enabled: box.selected !== ""
                    onClicked: box.view.remove(box.selected)
                }
            }
            Text {
                width: parent.width
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: Theme.bad
                visible: text !== ""
                text: box.view !== null ? box.view.problem : ""
            }
            Button {
                objectName: "workspaceOpen"
                text: qsTr("Open workspace")
                primary: true
                enabled: box.selected !== ""
                onClicked: {
                    if (box.view.restore(box.selected))
                        box.close();
                }
            }
        }
    }
}
