import QtQuick
import QtQuick.Layouts
import Qkvm

// The bar over the wall: one row, as on the page. At the left what narrows the wall, at the right
// what is done to it, then the small buttons and the time of day.
Rectangle {
    id: bar

    required property WallView wall
    required property Look look
    property SessionView session: null
    property bool workspacesAvailable: false
    // Whether what goes to the server is encrypted on its way: said on the bar where it is not.
    property bool encrypted: true
    // Whoever is signed in manages this QKVM: machines are added on its page, and the way there is
    // offered.
    property bool manages: false
    // The wall is this PC's own: a KVM is added in the program, and not on a server's page.
    property bool ownWall: false
    // The window fills the screen.
    property bool fullScreen: false
    // Whoever is signed in may act on machines: only then are machines picked for it.
    property bool selectable: false
    property bool lockAvailable: false
    property bool appLocked: false

    // Puts the keyboard into "Find a KVM".
    function find() {
        search.forceActiveFocus();
    }
    // Closes what drops from the bar: Qt does not close a drop with the bar, nor where its window
    // changes size, and room and rows were worked out for the window as it was.
    function closeMenus() {
        groupsMenu.close();
        viewMenu.close();
        moreMenu.close();
        accountMenu.close();
    }

    // What gives way as the window gets narrower, in this order: first what is only pleasant, then
    // the words on the buttons, and last the buttons that are least needed. Down to the narrowest
    // window there is, the bar stays one row and stays inside the window. The widths are measured
    // ones: a test holds the bar to them at every step (ui/tests/test_bar_screens.cpp), on a wall
    // whose count has two figures; where it has more, the filters keep more room for their counts,
    // and that is taken out of the room for the rest (room).
    readonly property real room: width - filters.beyondTwoFigures
    // (In a window too narrow for Settings, "Find a KVM" gives way by what More takes beside the
    // account there, and by the room the filters keep for counts of more than two figures.)
    readonly property real findGivesWay: roomForSettings ? 0 : 16 + filters.beyondTwoFigures
    readonly property bool roomForClock: room >= 1400
    readonly property bool roomForSelectWords: room >= 1270
    readonly property bool roomForName: room >= 1216
    readonly property bool roomForAddWords: room >= 1166
    readonly property bool roomForGroupWords: room >= 1092
    readonly property bool roomForViewWords: room >= 1002
    readonly property bool roomForStateWords: room >= 960
    readonly property bool roomForGroups: room >= 810
    readonly property bool roomForLesserButtons: room >= 767
    readonly property bool roomForSettings: room >= 690

    // No new pictures are asked for just now. Said on the bar: a wall that stands still looks like
    // one that is up to date.
    readonly property bool paused: wall.pace === 0
    // The group the wall is narrowed to: the chosen one where the wall has it, and otherwise none.
    readonly property string group: wall.machines.groups.includes(wall.shown.group) ? wall.shown.group : ""

    signal aboutAsked
    signal pageAsked
    signal fullScreenAsked
    signal settingsAsked
    signal windowAsked
    signal lockAsked
    signal sourcesAsked
    signal workspacesAsked
    // Esc emptied "Find a KVM" and left it: the keyboard is the wall's again.
    signal findLeft

    implicitHeight: Theme.controlHeight + 16
    // Gone (a machine is shown large, the wall only shown, nobody signed in): so is what dropped
    // from it, which would stand alone over what came, and hold the keys.
    onVisibleChanged: {
        if (!visible)
            closeMenus();
    }
    color: Theme.panel

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 8

        Button {
            ghost: true
            hint: Words.aboutQkvm
            leftPadding: 4
            rightPadding: 4
            onClicked: bar.aboutAsked()

            contentItem: Row {
                spacing: 9

                Mark {
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    visible: bar.roomForName
                    text: "qKVM"
                    color: Theme.text
                    font.pixelSize: Theme.titleSize
                    font.weight: Font.Bold
                }
            }
        }
        // As wide as the filters are with all four there, they at its left: one that comes or
        // goes moves nothing beside it, and "Find a KVM" stays where it stood.
        Item {
            implicitWidth: filters.widthWithAll
            implicitHeight: filters.implicitHeight

            Filters {
                id: filters

                anchors.verticalCenter: parent.verticalCenter
                machines: bar.wall.machines
                chosen: bar.wall.shown.show
                words: bar.roomForStateWords
                // (Counts of thousands, in the narrow window: "1.2k". Of the whole window's width,
                // not of the room it leaves, which goes by them.)
                compact: bar.width < 767
                onPicked: what => bar.wall.shown.show = what
            }
        }
        SearchField {
            id: search

            objectName: "findAKvm"
            // (Narrower, where the window is too narrow for Settings, by the room the filters keep
            // for counts of more than two figures: nothing else is left to give way there.)
            Layout.minimumWidth: Math.max(48, 96 - bar.findGivesWay)
            Layout.preferredWidth: Math.max(48, implicitWidth - bar.findGivesWay)
            placeholderText: Words.findAKvm
            hint: Words.findByNameOrAddress
            text: bar.wall.shown.find
            onTextEdited: bar.wall.shown.find = text
            // Esc empties it and leaves it, as on the page (app.js, the search's keydown): the
            // wall is all there again, and the keyboard is the wall's. (Before the window's own
            // Esc, which would leave picking.)
            Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape
            Keys.onEscapePressed: {
                bar.wall.shown.find = "";
                bar.findLeft();
            }
        }
        // Only where the wall's machines are in groups at all.
        Button {
            objectName: "groupsButton"
            visible: bar.roomForGroups && bar.wall.machines.groups.length > 0
            symbol: "grid"
            text: bar.roomForGroupWords ? (bar.group !== "" ? bar.group : Words.allGroups) : ""
            hint: Words.group
            drops: true
            checked: groupsMenu.opened
            onClicked: groupsMenu.toggle()

            Menu {
                id: groupsMenu

                objectName: "groupsMenu"
                rows: [
                    {
                        "text": Words.allGroups,
                        "kind": "group",
                        "value": "",
                        "chosen": bar.group === ""
                    }
                ].concat(bar.wall.machines.groups.map(name => ({
                            "text": name,
                            "kind": "group",
                            "value": name,
                            "chosen": bar.group === name
                        })))
                onPicked: (kind, value) => bar.wall.shown.group = value
            }
        }
        Item {
            Layout.fillWidth: true
        }
        Row {
            objectName: "barNotEncrypted"
            visible: !bar.encrypted
            spacing: 6
            Accessible.role: Accessible.StaticText
            Accessible.name: Words.notEncryptedShort
            Accessible.description: bar.ownWall ? Words.notEncryptedHintOwnWall : Words.notEncryptedHint

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "alert"
                color: Theme.warn
            }
            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                // In words where there is room for them; the sign alone where there is not.
                visible: bar.roomForClock
                text: Words.notEncryptedShort
                color: Theme.warn
                font.pixelSize: Theme.smallSize
            }
        }
        Button {
            objectName: "viewButton"
            symbol: bar.paused ? "pause" : "columns"
            noted: bar.paused
            text: bar.roomForViewWords ? (bar.paused ? Words.paused : Words.view) : ""
            drops: true
            hint: bar.paused ? Words.picturesArePaused : Words.howTheWallIsShown
            checked: viewMenu.opened
            onClicked: viewMenu.toggle()

            ViewMenu {
                id: viewMenu

                objectName: "viewMenu"
                wall: bar.wall
                manages: bar.manages
                onPicked: (kind, value) => {
                    if (kind === "order")
                        bar.wall.shown.order = value;
                    else if (kind === "columns")
                        bar.wall.columns = value;
                    else if (kind === "pace")
                        bar.wall.pace = value;
                    else if (kind === "window")
                        bar.windowAsked();
                    else if (kind === "arrange")
                        bar.wall.arrange();
                    else
                        bar.wall.displaying = true;
                }
            }
        }
        Button {
            objectName: "selectButton"
            visible: bar.selectable
            symbol: "checkSquare"
            text: bar.roomForSelectWords ? Words.select : ""
            hint: Words.selectKvms
            checked: bar.wall.selecting
            onClicked: bar.wall.selecting = !bar.wall.selecting
        }
        Button {
            objectName: "addButton"
            visible: bar.manages
            primary: true
            symbol: "plus"
            text: bar.roomForAddWords ? Words.addKvms : ""
            hint: bar.ownWall ? Words.addAKvmToThisPc : Words.addKvmsOnThePage
            onClicked: bar.pageAsked()
        }
        Rectangle {
            Layout.fillHeight: true
            Layout.topMargin: 12
            Layout.bottomMargin: 12
            implicitWidth: 1
            color: Theme.lineStrong
        }
        Button {
            objectName: "fullScreenButton"
            visible: bar.roomForLesserButtons
            symbol: bar.fullScreen ? "minimize" : "maximize"
            hint: bar.fullScreen ? Words.leaveFullscreen : Words.fullscreen
            onClicked: bar.fullScreenAsked()
        }
        Button {
            objectName: "themeButton"
            visible: bar.roomForLesserButtons
            symbol: bar.look.themeSymbol
            hint: bar.look.themeHint
            onClicked: bar.look.nextTheme()
        }
        Button {
            objectName: "settingsButton"
            visible: bar.roomForSettings
            symbol: "sliders"
            hint: Words.settings
            onClicked: bar.settingsAsked()
        }
        // What gave way as the window got narrower, in the order it did, in a menu at the end of
        // the bar: the bar stays one row, and nothing of it is out of reach. (The page wraps to
        // a second row on a phone; a program's window has its menus instead.)
        Button {
            id: moreButton

            objectName: "moreButton"
            visible: moreMenu.rows.length > 0
            symbol: "more"
            hint: Words.more
            checked: moreMenu.opened
            onClicked: moreMenu.toggle()

            Menu {
                id: moreMenu

                objectName: "moreMenu"
                fromTheRight: true
                rows: {
                    const groups = bar.roomForGroups || bar.wall.machines.groups.length === 0 ? [] : [
                        {
                            "heading": Words.group
                        },
                        {
                            "text": Words.allGroups,
                            "kind": "group",
                            "value": "",
                            "chosen": bar.group === ""
                        }
                    ].concat(bar.wall.machines.groups.map(name => ({
                                "text": name,
                                "kind": "group",
                                "value": name,
                                "chosen": bar.group === name
                            })));
                    const lesser = bar.roomForLesserButtons ? [] : [
                        {
                            "text": bar.fullScreen ? Words.leaveFullscreen : Words.fullscreen,
                            "kind": "fullScreen",
                            "value": true,
                            "hint": "F11"
                        },
                        {
                            "text": bar.look.themeHint,
                            "kind": "theme",
                            "value": true
                        }
                    ];
                    const settings = bar.roomForSettings ? [] : [
                        {
                            "text": Words.settings,
                            "kind": "settings",
                            "value": true,
                            "hint": "Ctrl+,"
                        }
                    ];
                    // (A line between what came from one place of the bar and what from another.)
                    return [groups, lesser, settings].filter(part => part.length > 0).reduce((all, part) => all.length === 0 ? part : all.concat([
                            {
                                "divider": true
                            }
                        ], part), []);
                }
                onPicked: (kind, value) => {
                    if (kind === "group")
                        bar.wall.shown.group = value;
                    else if (kind === "fullScreen")
                        bar.fullScreenAsked();
                    else if (kind === "theme")
                        bar.look.nextTheme();
                    else
                        bar.settingsAsked();
                }
            }
        }
        // More's place, kept where the window is too narrow for the groups and nothing gave way
        // but them (a wall without groups): a wall's first group to come moves nothing.
        Item {
            objectName: "moreSlot"
            visible: !bar.roomForGroups && !moreButton.visible
            Layout.preferredWidth: Theme.controlHeight
            Layout.preferredHeight: Theme.controlHeight
        }
        Button {
            objectName: "accountButton"
            symbol: "users"
            hint: Words.yourAccount
            checked: accountMenu.opened
            onClicked: accountMenu.toggle()

            AccountMenu {
                id: accountMenu

                objectName: "accountMenu"
                session: bar.session
                lockAvailable: bar.lockAvailable
                appLocked: bar.appLocked
                workspacesAvailable: bar.workspacesAvailable
                onPicked: kind => {
                    if (kind === "sources")
                        bar.sourcesAsked();
                    else if (kind === "workspaces")
                        bar.workspacesAsked();
                    else
                        bar.lockAsked();
                }
            }
        }
        Text {
            textFormat: Text.PlainText
            visible: bar.roomForClock
            Layout.minimumWidth: 78
            horizontalAlignment: Text.AlignRight
            // Read only while it is seen: a clock nobody sees (a narrow window, a wall that is
            // only shown) would still have the window drawn once a second.
            text: visible ? bar.wall.clock : ""
            color: Theme.muted
            font.pixelSize: Theme.textSize
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.lineStrong
    }
}
