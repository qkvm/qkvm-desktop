pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

Dialog {
    id: box
    required property SessionView session
    property var collapsed: ({})
    readonly property var rows: session !== null ? session.sourceTree : []
    readonly property var shownRows: {
        const query = search.text.trim().toLocaleLowerCase();
        if (query !== "") {
            const keep = {};
            for (let i = 0; i < rows.length; ++i) {
                const row = rows[i];
                if (row.label.toLocaleLowerCase().includes(query)) {
                    keep[row.key] = true;
                    keep[row.parentKey] = true;
                    keep[row.ancestorKey] = true;
                    if (row.depth < 2) {
                        for (let j = i + 1; j < rows.length && rows[j].depth > row.depth; ++j)
                            keep[rows[j].key] = true;
                    }
                }
            }
            return rows.filter(row => keep[row.key]);
        }
        return rows.filter(row => !collapsed[row.parentKey] && !collapsed[row.ancestorKey]);
    }
    function toggle(key) {
        const next = Object.assign({}, collapsed);
        next[key] = !next[key];
        collapsed = next;
    }
    title: qsTr("Sources")
    width: Math.min(620, parent ? parent.width - 24 : 620)
    onOpened: search.forceActiveFocus()
    onClosed: search.text = ""
    Connections {
        target: box.session
        function onSourcesChanged() {
            if (!box.session.canChooseSource) {
                box.collapsed = ({});
                box.close();
            }
        }
    }
    Column {
        width: box.availableWidth
        spacing: 12
        Text {
            width: parent.width
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.muted
            text: qsTr("Browse all saved sources. Open a source or machine in this window; the other windows keep their own view.")
        }
        SearchField {
            id: search
            objectName: "sourceSearch"
            width: parent.width
            hint: qsTr("Find a source, group or machine")
            placeholderText: hint
        }
        ListView {
            id: tree
            objectName: "sourceTree"
            width: parent.width
            height: Math.max(110, Math.min(420, box.parent ? box.parent.height - 280 : 420))
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: box.shownRows
            ScrollMark {
                flickable: tree
            }
            spacing: 3
            delegate: Item {
                id: row
                required property var modelData
                width: tree.width
                height: 54
                Button {
                    id: expand
                    objectName: "sourceExpand-" + row.modelData.key
                    x: row.modelData.depth * 18
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.modelData.depth < 2
                    symbol: search.text.trim() === "" && box.collapsed[row.modelData.key] ? "chevronRight" : "chevronDown"
                    hint: box.collapsed[row.modelData.key] ? qsTr("Expand %1").arg(row.modelData.label) : qsTr("Collapse %1").arg(row.modelData.label)
                    ghost: true
                    onClicked: box.toggle(row.modelData.key)
                }
                Button {
                    objectName: "sourceRow-" + row.modelData.key
                    x: row.modelData.depth * 18 + 38
                    width: parent.width - x
                    height: parent.height
                    text: row.modelData.label
                    hint: row.modelData.status
                    ghost: true
                    checked: row.modelData.depth === 0 && row.modelData.source === box.session.source
                    contentItem: Column {
                        spacing: 3
                        Text {
                            width: parent.width
                            textFormat: Text.PlainText
                            text: row.modelData.label
                            color: Theme.text
                            font.pixelSize: Theme.textSize
                            font.weight: row.modelData.depth === 0 ? Font.DemiBold : Font.Normal
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            textFormat: Text.PlainText
                            text: row.modelData.status
                            color: Theme.muted
                            font.pixelSize: Theme.textSize - 1
                            elide: Text.ElideRight
                        }
                    }
                    onClicked: {
                        if (row.modelData.depth === 1) {
                            box.toggle(row.modelData.key);
                        } else {
                            if (row.modelData.depth === 0)
                                box.session.chooseSource(row.modelData.source);
                            else
                                box.session.openMachine(row.modelData.scope, row.modelData.machine);
                            box.close();
                        }
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: tree.count === 0
                textFormat: Text.PlainText
                text: qsTr("No matching sources or machines")
                color: Theme.muted
            }
        }
        Button {
            text: qsTr("Connect to another server…")
            onClicked: {
                box.session.chooseSource("connect");
                box.close();
            }
        }
    }
}
