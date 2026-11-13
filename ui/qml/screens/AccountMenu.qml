pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// Your account, dropping from the bar's last button as the page's does (index.html, "me-panel"):
// who is signed in and where, a line, then what is done for this account and this PC, and last the
// way out. At this PC's own wall nobody is signed in: whose the wall is, and the way to a qKVM
// server. What the program has that the page has not stands here too: the source of this window
// (Ctrl+Shift+S browses all of them), the saved workspaces (Ctrl+Shift+W) and the app lock
// (Ctrl+L). A row that leads somewhere closes it first. (A server's wall as a file is in the
// settings, as on the page.)
Panel {
    id: menu

    required property SessionView session
    property bool lockAvailable: false
    property bool appLocked: false
    property bool workspacesAvailable: false

    // (As wide as its rows with their keys need: wider than the page's, which has no keys.)
    widest: Math.max(236, browse.implicitWidth, workspaces.implicitWidth, lock.implicitWidth)
    // (As a row was chosen: "sources", "workspaces" or "lock".)
    first: [sourceChoice, browse, workspaces, lock, signOut, toAServer].find(one => one.visible) ?? null

    // Nobody is signed in any more: there is no account to show.
    Connections {
        function onChanged() {
            if (menu.session.stage !== "wall")
                menu.close();
        }

        target: menu.session
    }

    Column {
        width: parent.width
        spacing: 1

        Text {
            objectName: "accountWho"
            textFormat: Text.PlainText
            width: parent.width
            text: menu.session.ownWall ? Words.thisPc : menu.session.account
            color: Theme.text
            font.pixelSize: Theme.textSize
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        // What the server lets them do, as the page's drop says it ("me-role").
        Text {
            objectName: "accountRole"
            textFormat: Text.PlainText
            width: parent.width
            visible: text !== ""
            text: ({
                    "admin": Words.admin,
                    "operator": Words.operator,
                    "viewer": Words.viewer,
                    "display": Words.aScreenOnTheWall
                })[menu.session.ownWall ? "" : menu.session.role] ?? ""
            color: Theme.muted
            font.pixelSize: Theme.textSize
            elide: Text.ElideRight
        }
        Text {
            objectName: "accountWhere"
            textFormat: Text.PlainText
            width: parent.width
            text: menu.session.ownWall ? Words.thisPcsOwnWall : Words.atTheQkvm.arg(menu.session.address)
            color: Theme.muted
            font.pixelSize: Theme.textSize
            wrapMode: menu.session.ownWall ? Text.WordWrap : Text.WrapAnywhere
        }
    }
    Panel.Divider {}
    Column {
        width: parent.width
        spacing: 5
        visible: menu.session.canChooseSource

        Panel.Heading {
            text: qsTr("Source for this window")
        }
        Choice {
            id: sourceChoice

            objectName: "sourceChoice"
            width: parent.width
            arrowsStep: true
            hint: qsTr("Source for this window")
            choices: menu.session.sources.map(one => ({
                        "value": one.value,
                        "label": one.label
                    }))
            value: menu.session.source
            onPicked: value => {
                menu.close();
                menu.session.chooseSource(value);
            }
        }
    }
    Panel.Row {
        id: browse

        objectName: "browseSources"
        visible: menu.session.canChooseSource
        symbol: "search"
        text: qsTr("Browse all sources…")
        key: "Ctrl+Shift+S"
        onClicked: menu.pick("sources", true)
    }
    Panel.Row {
        id: workspaces

        objectName: "savedWorkspaces"
        visible: menu.workspacesAvailable
        symbol: "archive"
        text: qsTr("Saved workspaces…")
        key: "Ctrl+Shift+W"
        onClicked: menu.pick("workspaces", true)
    }
    Panel.Row {
        id: lock

        objectName: "lockRow"
        visible: menu.lockAvailable
        symbol: "lock"
        text: menu.appLocked ? qsTr("Unlock qKVM") : qsTr("Lock qKVM")
        key: "Ctrl+L"
        onClicked: menu.pick("lock", true)
    }
    Panel.Divider {
        visible: sourceChoice.visible || browse.visible || workspaces.visible || lock.visible
    }
    Panel.Row {
        id: signOut

        objectName: "signOut"
        visible: !menu.session.ownWall
        symbol: "logOut"
        text: Words.signOut
        onClicked: {
            menu.close();
            menu.session.signOut();
        }
    }
    Panel.Row {
        id: toAServer

        objectName: "connectToAServer"
        visible: menu.session.ownWall
        symbol: "enter"
        text: Words.connectToAQkvmServer
        onClicked: {
            menu.close();
            menu.session.anotherServer();
        }
    }
}
