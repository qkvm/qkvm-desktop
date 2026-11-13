pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import Qkvm

Rectangle {
    id: panel
    required property WallView view
    required property string videoDescription
    readonly property DetailsView details: view.details
    readonly property var card: view.enlargedCard
    property bool allActivity: false
    signal closeAsked
    color: Theme.panel
    border.color: Theme.lineStrong
    radius: 6
    clip: true
    onVisibleChanged: if (!visible)
        allActivity = false

    RowLayout {
        id: heading
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 8
        Text {
            Layout.fillWidth: true
            text: qsTr("Details")
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.textSize
            font.bold: true
        }
        Button {
            objectName: "refreshConsoleDetails"
            text: panel.details.busy ? qsTr("Looking…") : qsTr("Refresh")
            enabled: panel.details.available && !panel.details.busy
            onClicked: panel.details.refresh()
        }
        Button {
            objectName: "closeConsoleDetails"
            text: Words.close
            onClicked: panel.closeAsked()
        }
    }
    Flickable {
        id: scroll
        objectName: "consoleDetailsScroll"
        anchors.top: heading.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        clip: true
        readonly property real availableWidth: width - 14
        activeFocusOnTab: true
        Accessible.name: qsTr("Machine details, history and activity")
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: availableWidth
        contentHeight: contents.height
        function moveTo(y: real) {
            contentY = Math.max(0, Math.min(y, contentHeight - height));
        }
        Keys.onPressed: event => {
            if (event.key === Qt.Key_PageDown || event.key === Qt.Key_Down)
                moveTo(contentY + (event.key === Qt.Key_Down ? 40 : height * 0.8));
            else if (event.key === Qt.Key_PageUp || event.key === Qt.Key_Up)
                moveTo(contentY - (event.key === Qt.Key_Up ? 40 : height * 0.8));
            else if (event.key === Qt.Key_Home)
                moveTo(0);
            else if (event.key === Qt.Key_End)
                moveTo(contentHeight);
            else
                return;
            event.accepted = true;
        }
        Column {
            id: contents
            width: scroll.availableWidth
            spacing: 12
            Text {
                objectName: "consoleDeviceDetails"
                width: parent.width
                text: [panel.card.name, panel.card.group, panel.card.host, panel.card.title, panel.card.meta, panel.card.note].filter(value => value).join("\n")
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.textSize
                wrapMode: Text.Wrap
            }
            Text {
                objectName: "videoDiagnosticsText"
                width: parent.width
                text: panel.videoDescription
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.smallSize
                wrapMode: Text.Wrap
            }
            Rectangle {
                width: parent.width
                height: 1
                color: Theme.lineStrong
            }
            Text {
                width: parent.width
                text: qsTr("History")
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.textSize
                font.bold: true
            }
            Text {
                objectName: "consoleHistorySummary"
                width: parent.width
                text: !panel.details.available || panel.details.own ? qsTr("Server history is available for a machine on a connected server.") : panel.details.historyProblem ? qsTr("The history could not be read: %1").arg(panel.details.historyProblem) : panel.details.summary.lastSeen === undefined ? qsTr("Looking at the history…") : qsTr("Last seen: %1").arg(panel.details.summary.lastSeen) + "\n" + (panel.details.summary.known ? qsTr("Online: %1 · %2").arg(panel.details.summary.online).arg(panel.details.summary.watched) : qsTr("Nothing seen of it this week yet"))
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.smallSize
                wrapMode: Text.Wrap
            }
            Repeater {
                model: panel.details.graphs
                delegate: HistoryGraph {
                    required property var modelData
                    width: scroll.availableWidth
                    values: modelData
                }
            }
            Text {
                width: parent.width
                visible: text !== ""
                text: panel.details.graphProblem || panel.details.graphNote
                textFormat: Text.PlainText
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                wrapMode: Text.Wrap
            }
            Column {
                objectName: "consoleActivity"
                visible: panel.details.administrator
                width: parent.width
                spacing: 8
                Text {
                    text: qsTr("Done to it")
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    font.bold: true
                }
                Repeater {
                    model: panel.allActivity ? panel.details.activity : panel.details.activity.slice(0, 3)
                    delegate: Column {
                        id: entry
                        required property var modelData
                        width: scroll.availableWidth
                        spacing: 4
                        Text {
                            width: parent.width
                            text: entry.modelData.when + " · " + entry.modelData.user + "\n" + entry.modelData.what
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.smallSize
                            wrapMode: Text.Wrap
                        }
                        // The picture that set off an alert of its screen watch, where one was kept: asked
                        // for only when it is looked at, as the page's "Picture" is.
                        Button {
                            objectName: "alertPictureOf" + entry.modelData.row
                            visible: entry.modelData.picture === true
                            implicitHeight: 24
                            text: qsTr("Picture")
                            onClicked: panel.details.showPicture(entry.modelData.row)
                        }
                    }
                }
                Text {
                    width: parent.width
                    visible: text !== ""
                    text: panel.details.activityProblem || panel.details.activityNote
                    textFormat: Text.PlainText
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    wrapMode: Text.Wrap
                }
                Button {
                    objectName: "moreConsoleActivity"
                    visible: panel.details.activity.length > 3
                    text: panel.allActivity ? qsTr("Show less") : qsTr("Show recent activity (%1)").arg(panel.details.activity.length)
                    onClicked: panel.allActivity = !panel.allActivity
                    onActiveFocusChanged: if (activeFocus) {
                        const bottom = mapToItem(contents, 0, height).y;
                        if (bottom > scroll.contentY + scroll.height)
                            scroll.moveTo(bottom - scroll.height);
                    }
                }
            }
        }
    }
    Rectangle {
        id: track
        anchors.top: scroll.top
        anchors.bottom: scroll.bottom
        anchors.right: scroll.right
        width: 8
        radius: 4
        color: Theme.inset
        visible: scroll.contentHeight > scroll.height
        Rectangle {
            width: parent.width
            height: Math.max(24, track.height * scroll.height / Math.max(1, scroll.contentHeight))
            y: (track.height - height) * scroll.contentY / Math.max(1, scroll.contentHeight - scroll.height)
            radius: 4
            color: Theme.muted
        }
        MouseArea {
            anchors.fill: parent
            function scrollAt(y: real) {
                scroll.moveTo(y / height * scroll.contentHeight - scroll.height / 2);
            }
            onPressed: mouse => {
                scroll.forceActiveFocus();
                scrollAt(mouse.y);
            }
            onPositionChanged: mouse => {
                if (pressed)
                    scrollAt(mouse.y);
            }
        }
    }
    // An alert's picture, over the details and as large as they are, whatever it holds: its name and
    // Close at the top, the picture as large as fits, and one line under it (when the wall saw it, or
    // why there is none). As the page's dialog of it.
    Rectangle {
        id: alertPicture
        objectName: "alertPictureBox"
        readonly property var said: panel.details.picture
        anchors.fill: parent
        visible: said.open === true
        color: Theme.panel
        radius: panel.radius
        border.color: Theme.lineStrong
        // (Nothing under it is reached through it.)
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => wheel.accepted = true
        }
        RowLayout {
            id: pictureHeading
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            spacing: 8
            Text {
                objectName: "alertPictureTitle"
                Layout.fillWidth: true
                text: [panel.card.name ?? "", alertPicture.said.title ?? ""].filter(part => part !== "").join(": ")
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.textSize
                font.bold: true
                elide: Text.ElideRight
            }
            Button {
                objectName: "closeAlertPicture"
                text: Words.close
                onClicked: panel.details.closePicture()
            }
        }
        Rectangle {
            id: pictureArea
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: pictureHeading.bottom
            anchors.bottom: pictureLine.top
            anchors.margins: 12
            color: Theme.screen
            radius: Theme.radius
            AlertPicture {
                objectName: "alertPicture"
                anchors.fill: parent
                image: panel.details.pictureImage
            }
            Spinner {
                anchors.centerIn: parent
                visible: alertPicture.said.state === "loading"
                size: 18
                color: Theme.screenMuted
            }
        }
        Text {
            id: pictureLine
            objectName: "alertPictureLine"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            height: 32
            text: alertPicture.said.state === "gone" ? qsTr("This picture is no longer kept: only the last alert's of each machine is, for 30 days.") : alertPicture.said.state === "failed" ? qsTr("The picture could not be had: %1").arg(alertPicture.said.problem ?? "") : alertPicture.said.state === "loading" ? qsTr("Looking…") : alertPicture.said.at ?? ""
            textFormat: Text.PlainText
            color: Theme.muted
            font.pixelSize: Theme.smallSize
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
    }
}
