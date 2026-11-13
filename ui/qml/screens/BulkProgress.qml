pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// One thing being done to several machines, as the page shows it: a line that says how far it is,
// each machine with how it went, and under them what can be done about it: stop it while it runs,
// send once more what can be sent once more, and close.
//
// It is opened and closed by the program. Closed by hand while it runs, that is the quickest way to
// say "stop": the program is told, and sends nothing more. The keyboard is at Stop while the run
// goes and at Close once it is over, never by itself at what would send anything once more.
Dialog {
    id: box

    required property ActionsView actions

    readonly property var run: actions.run
    readonly property bool there: run.title !== undefined
    readonly property bool running: run.running === true
    // As high as the window lets the box be, and what the box needs beside the list of the machines:
    // the list takes what is left of the one after the other, and scrolls.
    readonly property real room: (parent ? parent.height : 600) - 24
    readonly property real beside: implicitHeaderHeight + topPadding + bottomPadding + summary.height + buttons.height + 2 * 12

    title: run.title ?? ""
    width: Math.min(560, parent ? parent.width - 24 : 560)

    onThereChanged: {
        if (there)
            open();
        else
            close();
    }
    onClosed: {
        if (there)
            actions.closeRun();
    }
    onOpened: land()
    onRunningChanged: {
        if (opened)
            land();
    }

    function land() {
        (running ? stopButton : closeButton).forceActiveFocus();
    }

    Column {
        width: box.availableWidth
        spacing: 12

        Text {
            id: summary

            textFormat: Text.PlainText
            objectName: "runSummary"
            width: parent.width
            text: box.run.summary ?? ""
            color: Theme.text
            font.pixelSize: Theme.textSize
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StatusBar
            Accessible.name: text
        }
        // The machines, in the order they are done; the list scrolls where there are many.
        Rectangle {
            width: parent.width
            height: Math.min(rows.contentHeight, 280, Math.max(68, box.room - box.beside - 2)) + 2
            radius: Theme.radius
            color: Theme.inset
            border.width: 1
            border.color: Theme.line

            ListView {
                id: rows

                x: 1
                y: 1
                width: parent.width - 2
                height: parent.height - 2
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: box.run.rows ?? []

                delegate: Item {
                    id: row

                    required property var modelData
                    required property int index

                    objectName: "runRow"
                    width: rows.width
                    height: Math.max(34, said.implicitHeight + 14)

                    Rectangle {
                        visible: row.index > 0
                        width: parent.width
                        height: 1
                        color: Theme.line
                    }
                    Spinner {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        visible: row.modelData.state === ""
                    }
                    Icon {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        visible: row.modelData.state !== ""
                        size: 14
                        name: row.modelData.state === "ok" ? "check" : row.modelData.state === "bad" ? "alert" : row.modelData.state === "unknown" ? "info" : "x"
                        color: row.modelData.state === "ok" ? Theme.ok : row.modelData.state === "bad" ? Theme.bad : row.modelData.state === "unknown" ? Theme.warn : Theme.muted
                    }
                    Text {
                        id: named

                        textFormat: Text.PlainText
                        x: 33
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width * 0.45)
                        text: row.modelData.name
                        color: Theme.text
                        font.pixelSize: Theme.textSize
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        id: said

                        textFormat: Text.PlainText
                        anchors.left: named.right
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignRight
                        text: row.modelData.text
                        color: Theme.muted
                        font.pixelSize: Theme.smallSize
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
        Item {
            id: buttons

            width: parent.width
            height: Theme.controlHeight

            Row {
                anchors.right: parent.right
                spacing: 8

                Button {
                    objectName: "runAgain"
                    visible: (box.run.again ?? "") !== ""
                    text: box.run.again ?? ""
                    onClicked: box.actions.retryRun()
                }
                Button {
                    id: stopButton

                    objectName: "runStop"
                    visible: box.running
                    enabled: box.run.stopping !== true
                    danger: true
                    text: Words.stop
                    onClicked: box.actions.stopRun()
                }
                Button {
                    id: closeButton

                    objectName: "runClose"
                    visible: !box.running
                    primary: true
                    text: Words.close
                    onClicked: box.actions.closeRun()
                }
            }
        }
    }
}
