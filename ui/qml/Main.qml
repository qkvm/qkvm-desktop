pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// The program's window. Until somebody is signed in to a QKVM: which one, and who. From then on the
// bar, and under it the wall, laid out as the page is (SPEC.md section 5).
// Whoever makes the window shows it, after the engine was given to it: it is not visible by itself.
Window {
    id: window

    required property AboutModel about
    required property SessionView session
    property WorkspacesView workspaces: null
    required property WallView wall
    required property ActionsView actions
    required property Look look
    required property SettingsView settings
    required property FilesView files
    required property OwnFilesView ownFiles
    required property AddKvmView adding
    required property EditKvmView editing
    required property ControlView control
    // This PC's KVMs as the settings list them: one for the program.
    required property OwnKvmsView kvms
    readonly property AppLock security: settings.security

    readonly property bool walled: session.stage === "wall"
    // One machine is shown large, in the wall's place.
    readonly property bool enlarged: walled && wall.enlarged !== ""
    readonly property bool screenshotHoldsWindow: (enlargedLoader.item as Enlarged)?.screenshotHoldsWindow ?? false
    // The title of a box of this window with changes that are not saved; empty: none. The program does not close for an
    // update while one is (WallWindows::whatHoldsAnUpdate).
    readonly property string unsavedEdit: addKvmBox.unsaved ? addKvmBox.title : editKvmBox.unsaved ? editKvmBox.title : textBox.unsaved ? textBox.title : settingsBox.unsavedEdit
    property bool consoleOnTop: false
    readonly property bool consolePinAllowed: enlarged && !displaying && visibility !== Window.FullScreen && (security === null || !security.locked)
    flags: Qt.Window | (consolePinAllowed && consoleOnTop ? Qt.WindowStaysOnTopHint : 0)
    onConsolePinAllowedChanged: {
        if (!consolePinAllowed)
            consoleOnTop = false;
    }

    Binding {
        target: window.control
        property: "releaseKey"
        value: window.settings.releaseKey
    }

    // The wall is only shown, for a monitor on the wall: no bar, as the page's display mode.
    readonly property bool displaying: walled && wall.displaying
    // A box lies over the wall.
    readonly property bool boxOpen: sourcesBox.opened || workspacesBox.opened || appLockBox.opened || aboutBox.opened || exportBox.opened || importBox.opened || ownExportBox.opened || ownImportBox.opened || settingsBox.opened || machineMenu.opened || keysBox.opened || textBox.opened || runBox.opened || questionBox.opened || addKvmBox.opened || editKvmBox.opened || mediaBox.opened

    // The wall's own order is being made by hand: the cards are moved, and nothing else is done.
    readonly property bool arranging: walled && wall.arranging

    // The wall's own keys are the wall's: not a machine's that is shown large. (While a box or a
    // menu is open they rest anyway: Qt gives a window's keys to what lies over it. And while the
    // wall's order is being made they do nothing: the wall takes none of what they ask.)
    readonly property bool wallHasTheKeys: walled && !enlarged

    // The group this window's wall is narrowed to, where the wall has it; and otherwise none.
    readonly property string group: walled && wall.machines.groups.includes(wall.shown.group) ? wall.shown.group : ""

    // Whether it filled the desktop before it filled the screen: that is what it goes back to.
    property bool wasMaximized: false
    // How long after a click on a card showed its machine large a click is the second of a double
    // click: as long as Windows says one takes. (A test makes it longer, to be sure of its own.)
    property int secondClickMs: Application.styleHints.mouseDoubleClickInterval

    // Another window of the wall is wanted: whoever keeps the program's windows opens one.
    signal windowAsked

    // Over the whole screen without a frame, and back: the bar's button, and F11 as everywhere.
    function toggleFullScreen() {
        if (visibility === Window.FullScreen) {
            visibility = wasMaximized ? Window.Maximized : Window.Windowed;
        } else {
            wasMaximized = visibility === Window.Maximized;
            visibility = Window.FullScreen;
        }
    }

    width: 1280
    height: 800
    minimumWidth: 640
    minimumHeight: 400
    // A window that shows one group says which: several windows are told apart in the taskbar.
    // And one whose keyboard goes to a machine says which machine: its name is whole here,
    // however long it is, where its bar may have to cut it.
    title: enlarged && control.on ? Words.machineInTheTitle.arg(wall.enlargedCard.name ?? "") : (group !== "" ? Words.appNameWithGroup.arg(group) : Words.appName) + (session.canChooseSource ? " — " + (session.ownWall ? Words.thisPc : session.address) : "")
    color: Theme.bg

    // A box is opened while a machine's keyboard and mouse are asked for: whoever sits here went
    // on to something else, and what they type there is not the machine's when its yes comes.
    onBoxOpenChanged: {
        if (boxOpen)
            control.wentOn();
    }

    // A window that is minimized, or out of sight behind the program's icon, shows nobody anything.
    Binding {
        target: window.wall
        property: "unseen"
        value: window.visibility === Window.Hidden || window.visibility === Window.Minimized
    }
    // The groups of a wall that is only shown wait their turn while a box lies over it.
    Binding {
        target: window.wall
        property: "covered"
        value: window.boxOpen
    }

    Glow {
        anchors.fill: parent
    }
    // The bar goes, and the keyboard was at one of its buttons: it goes to the wall, and not to
    // whatever comes next, which would be a button of display mode's small bar that nobody sees.
    // And the small bar shows for a moment, as on the page: that is where the way out is.
    // Somebody has signed in: the keyboard is the wall's from that moment. It would otherwise stay
    // in the line the password was typed into, which nobody sees any more: the wall's own keys
    // would be letters typed there, and its mark would go on blinking, a frame drawn each time.
    // And whoever is not at the wall any more has no box of a file left open, with what was
    // typed into it: of a server's wall, or of this PC's own. (The account's box closes itself.)
    // (A turn later, once the wall is in sight. Where a box lies over the window then, the box
    // keeps the keyboard until it goes: Wall.qml.)
    onWalledChanged: {
        if (walled) {
            Qt.callLater(wallItem.takeTheKeyboard, "wall");
        } else {
            exportBox.close();
            importBox.close();
            ownExportBox.close();
            ownImportBox.close();
        }
    }
    // Nor is a KVM added to this PC's wall, or one of its KVMs changed, from a window that shows
    // a server's, or that asks for one. Nor is this PC's wall written to a file from there, or a
    // file's KVMs taken onto it: what was laid out of a file is forgotten with its box.
    Connections {
        target: window.session

        function onChanged() {
            if (!window.session.ownWall) {
                addKvmBox.close();
                editKvmBox.close();
                ownExportBox.close();
                ownImportBox.close();
            }
        }
    }
    // A machine's menu, or the button on its card, leads to the box that changes a KVM of this
    // PC's wall: the program fills the box and opens it.
    Connections {
        target: window.actions

        function onEditAsked(machine: string, review: bool) {
            if (window.session.ownWall)
                window.editing.openFor(machine, review);
        }
    }
    onDisplayingChanged: {
        if (displaying) {
            Qt.callLater(wallItem.takeTheKeyboard, "wall");
            displayBar.wake();
        }
    }

    Wall {
        id: wallItem

        objectName: "wall"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: banner.bottom
        anchors.bottom: parent.bottom
        visible: window.walled && !window.enlarged
        view: window.wall
        actions: window.actions
        old: banner.said
        ownWall: window.session.ownWall
        onAddAsked: addKvmBox.open()
        onOpenedByAClick: secondClick.restart()
    }
    // There only while a machine is shown large: a place for a picture that nobody sees is one
    // more for the engine to keep.
    Loader {
        id: enlargedLoader

        objectName: "enlarged"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: banner.bottom
        anchors.bottom: parent.bottom
        active: window.enlarged

        sourceComponent: Enlarged {
            view: window.wall
            actions: window.actions
            control: window.control
            externalPanel: window.boxOpen
            old: banner.said
            alwaysOnTop: window.consoleOnTop && window.consolePinAllowed
            canPin: window.consolePinAllowed
            onPinAsked: window.consoleOnTop = !window.consoleOnTop
            fullScreen: window.visibility === Window.FullScreen
            onFullScreenAsked: window.toggleFullScreen()
        }
    }
    // A double click on a card shows its machine large with its first click, as one click does;
    // its second lands where the card was, where the machine shown large has its buttons now. It
    // is no click on them. (Before one click did, a double click opened a card, and a hand used
    // to that, or to Windows, still gives one.)
    MouseArea {
        objectName: "secondClickOfAnOpening"
        anchors.fill: enlargedLoader
        enabled: secondClick.running && window.enlarged
        acceptedButtons: Qt.AllButtons

        Timer {
            id: secondClick

            interval: window.secondClickMs
        }
        // Another machine, or none: what is clicked now is no second click of that one.
        Connections {
            target: window.wall
            function onEnlargedChanged() {
                secondClick.stop();
            }
        }
    }
    // After the wall, and so over it: a card that is scrolled up goes under these two.
    // The server does not answer: the wall shows what it last knew, and says so and since when.
    Rectangle {
        id: banner

        readonly property bool said: window.walled && !window.session.inContact

        objectName: "banner"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: window.enlarged || window.displaying ? parent.top : bar.bottom
        height: said ? 30 : 0
        visible: said
        color: Theme.panel

        // A click on it is its own: a card scrolled up under it is not clicked by it.
        MouseArea {
            anchors.fill: parent
        }
        Rectangle {
            anchors.fill: parent
            color: Theme.badSoft
        }
        Text {
            textFormat: Text.PlainText
            anchors.centerIn: parent
            text: Words.lostContactAt.arg(window.session.awaySince)
            color: Theme.text
            font.pixelSize: Theme.smallSize
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.bad
            opacity: 0.4
        }
    }
    // A click on the bar where it has no button, or on the bar while it rests, is the bar's: a
    // card scrolled up under it is not clicked by it. (Beside the bar and not in it: a bar that
    // rests takes no click itself.)
    MouseArea {
        anchors.fill: bar
        visible: bar.visible
    }
    // The window changes size: what dropped from the bar was laid out for the window as it was (its
    // rows, its height), and closes.
    onWidthChanged: bar.closeMenus()
    onHeightChanged: bar.closeMenus()
    TopBar {
        id: bar

        objectName: "topBar"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        visible: window.walled && !window.enlarged && !window.displaying
        // While the wall's order is being made the bar rests, as on the page: every machine is
        // shown, and how they are shown is not chosen meanwhile.
        enabled: !window.arranging
        opacity: enabled ? 1 : 0.5
        wall: window.wall
        session: window.session
        workspacesAvailable: window.workspaces !== null && window.workspaces.allowed
        look: window.look
        encrypted: window.session.encrypted
        manages: window.session.manages
        ownWall: window.session.ownWall
        selectable: window.actions.allowed
        fullScreen: window.visibility === Window.FullScreen
        onAboutAsked: aboutBox.open()
        onPageAsked: window.session.ownWall ? addKvmBox.open() : window.session.openPage()
        onFullScreenAsked: window.toggleFullScreen()
        onSettingsAsked: settingsBox.open()
        onWindowAsked: window.windowAsked()
        onWorkspacesAsked: workspacesBox.open()
        onSourcesAsked: sourcesBox.open()
        onFindLeft: wallItem.forceActiveFocus()
        lockAvailable: window.security !== null
        appLocked: window.security !== null && window.security.locked
        onLockAsked: window.security.locked ? window.security.ask("unlock") : window.security.lock()
    }

    SourcesBox {
        id: sourcesBox
        objectName: "sourcesBox"
        session: window.session
    }
    Shortcut {
        sequence: "Ctrl+Shift+S"
        enabled: window.session !== null && window.session.canChooseSource && !window.control.on
        onActivated: sourcesBox.open()
    }
    Button {
        objectName: "startSources"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        visible: !window.walled && window.session !== null && window.session.canChooseSource
        text: qsTr("Sources…")
        onClicked: sourcesBox.open()
    }
    WorkspacesBox {
        id: workspacesBox
        objectName: "workspacesBox"
        view: window.workspaces
    }
    Button {
        objectName: "startWorkspaces"
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 16
        visible: !window.walled && window.workspaces !== null && window.workspaces.allowed
        text: qsTr("Saved workspaces…")
        onClicked: workspacesBox.open()
    }
    Shortcut {
        sequence: "Ctrl+Shift+W"
        enabled: window.workspaces !== null && window.workspaces.allowed && !window.control.on
        onActivated: workspacesBox.open()
    }

    // Display mode's own small bar, which shows for a moment whenever the pointer moves.
    Item {
        anchors.fill: parent

        HoverHandler {
            // (Qt says "changed" also when it looks again at where the pointer rests.)
            property point last

            enabled: window.displaying
            onPointChanged: {
                const at = point.scenePosition;
                if (at.x !== last.x || at.y !== last.y) {
                    last = at;
                    displayBar.wake();
                }
            }
        }
    }
    DisplayBar {
        id: displayBar

        objectName: "displayBar"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 12
        visible: window.displaying && !window.enlarged
        wall: window.wall
    }

    // What is there while machines are being picked, at the foot of the wall.
    BulkBar {
        objectName: "bulkBar"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 14
        visible: window.walled && !window.enlarged && window.wall.selecting
        wall: window.wall
        onRunAsked: window.actions.openPicked()
    }

    // And while the wall's own order is being made: what is being done, the way out, and saving it.
    ArrangeBar {
        objectName: "arrangeBar"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 14
        visible: window.arranging
        wall: window.wall
    }
    Shortcut {
        sequence: "Esc"
        enabled: window.arranging && !window.boxOpen
        onActivated: window.wall.stopArranging()
    }

    Shortcut {
        sequences: [StandardKey.FullScreen]
        onActivated: window.toggleFullScreen()
    }
    // And F, as the page's F (app.js, the document's keydown), on the wall and over a machine shown
    // large. Not while that machine's keyboard and mouse are taken (every key is the machine's
    // then, also while a panel of the console's own has the keys back for a moment), nor while
    // the wall's order is made (the wall's keys rest then, as on the page). Where something is
    // typed it is a letter, and while a box or a menu is open the box's.
    Shortcut {
        sequence: "F"
        // (Held, it does it once: the screen does not flicker by its repeat. And not while the
        // keyboard on the screen or the details of a machine shown large are open: theirs.)
        autoRepeat: false
        enabled: window.walled && !window.control.on && !window.arranging && !((enlargedLoader.item as Enlarged)?.panelOpen ?? false)
        onActivated: window.toggleFullScreen()
    }
    // The program's own settings, wherever it is: the bar's button is not there on every screen,
    // nor in a narrow window.
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: settingsBox.open()
    }
    Shortcut {
        sequence: "Ctrl+L"
        enabled: window.security !== null
        onActivated: window.security.locked ? window.security.ask("unlock") : window.security.lock()
    }
    Connections {
        target: window.security
        function onChanged() {
            if (window.security !== null && window.security.restricted) {
                settingsBox.close();
                addKvmBox.close();
                editKvmBox.close();
                exportBox.close();
                importBox.close();
                ownExportBox.close();
                ownImportBox.close();
            }
        }
    }
    AppLockBox {
        id: appLockBox
        security: window.security
    }
    Button {
        objectName: "appLockButton"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        z: 20
        visible: window.security !== null && (!window.walled || window.displaying || window.security.restricted)
        text: window.security !== null && window.security.locked ? qsTr("Unlock qKVM") : window.security !== null && window.security.wallScreen ? qsTr("Sign out wall screen") : qsTr("Lock qKVM")
        hint: "Ctrl+L"
        onClicked: {
            if (window.security.locked)
                window.security.ask("unlock");
            else if (window.security.wallScreen)
                window.security.ask("endScreen");
            else
                window.security.lock();
        }
    }
    // Another window of the wall, as a program's "New window" is everywhere: also where the wall is
    // only shown and has no bar to ask in.
    Shortcut {
        sequence: "Ctrl+N"
        enabled: window.walled
        onActivated: window.windowAsked()
    }
    // The wall's own keys, as the page has them: S picks machines for something done to several of
    // them and Esc leaves that, "/" goes to "Find a KVM". Where something is being typed, a letter
    // is a letter; and while a box or a menu is open, the keys are that one's.
    Shortcut {
        sequence: "S"
        enabled: window.wallHasTheKeys && window.actions.allowed
        onActivated: window.wall.selecting = !window.wall.selecting
    }
    Shortcut {
        sequence: "Esc"
        enabled: window.wallHasTheKeys && window.wall.selecting
        onActivated: window.wall.selecting = false
    }
    Shortcut {
        sequence: "/"
        enabled: window.wallHasTheKeys && !window.displaying
        onActivated: bar.find()
    }
    // D shows the wall and nothing else, and D or Esc brings the bar back.
    Shortcut {
        sequence: "D"
        enabled: window.wallHasTheKeys
        onActivated: window.wall.displaying = !window.wall.displaying
    }
    Shortcut {
        sequence: "Esc"
        enabled: window.wallHasTheKeys && window.wall.displaying && !window.wall.selecting
        onActivated: window.wall.displaying = false
    }

    Connect {
        objectName: "connect"
        anchors.fill: parent
        visible: !window.walled
        session: window.session
    }

    About {
        id: aboutBox

        objectName: "aboutBox"
        about: window.about
    }
    // The program's settings, and this PC's wall's. Before the boxes its sections open, which stand
    // over it, as the page stacks them, and close back to it.
    SettingsBox {
        id: settingsBox

        objectName: "settingsBox"
        settings: window.settings
        look: window.look
        kvms: window.kvms
        about: window.about
        session: window.session
        onAddAsked: addKvmBox.open()
        onEditAsked: machine => window.editing.openFor(machine, false)
        onRemoveAsked: machine => window.editing.openToRemove(machine)
        onExportAsked: ownExportBox.open()
        onImportAsked: ownImportBox.open()
        // (Of a server's wall: the server writes and reads the file of its own; and its boxes go
        // with the settings where the window comes to show another wall.)
        onServerExportAsked: exportBox.open()
        onServerImportAsked: importBox.open()
        onClosedForAnotherWall: {
            exportBox.close();
            importBox.close();
        }
        // (And where the window no longer shows a wall whose file is the signed-in one's to write
        // and read, whether the box is open or not: Settings may have gone before, for a notice's
        // machine, and left a server's file box over it.)
        onShowsChanged: {
            if (settingsBox.shows !== "managed") {
                exportBox.close();
                importBox.close();
            }
        }
    }
    // A taking that its box gave up is said where it is seen: it may have come in all the same.
    Connections {
        target: window.files
        function onCutOff(said: string) {
            window.actions.tell(said, false);
        }
    }
    Connections {
        target: window.settings
        function onIncidentsAsked() {
            if (window.active)
                settingsBox.open();
        }
    }
    Connections {
        target: window.settings.alerts
        function onOpenAsked(scope: string) {
            settingsBox.close();
        }
    }
    Connections {
        target: window.wall
        function onRevealAsked() {
            settingsBox.close();
            aboutBox.close();
            bar.closeMenus();
            // (And the boxes that lay over the wall go with it, where they can: not one whose work
            // cannot be given up, which says what came of it itself.)
            for (const box of [exportBox, importBox, ownExportBox, ownImportBox, addKvmBox, editKvmBox]) {
                if (box.canClose)
                    box.close();
            }
        }
    }
    ExportBox {
        id: exportBox

        objectName: "exportBox"
        files: window.files
        actions: window.actions
    }
    ImportBox {
        id: importBox

        objectName: "importBox"
        files: window.files
        actions: window.actions
    }
    // The same two for this PC's own wall.
    OwnExportBox {
        id: ownExportBox

        objectName: "ownExportBox"
        files: window.ownFiles
        actions: window.actions
    }
    OwnImportBox {
        id: ownImportBox

        objectName: "ownImportBox"
        files: window.ownFiles
        actions: window.actions
    }
    // A KVM is added to this PC's own wall.
    AddKvm {
        id: addKvmBox

        objectName: "addKvm"
        adding: window.adding
        actions: window.actions
        discoveryAllowed: window.walled && window.session.ownWall && !window.displaying && window.visibility !== Window.Hidden && window.visibility !== Window.Minimized
    }
    // And one of its KVMs is changed, or taken off it. Where its KVM was taken off elsewhere while
    // it was open, the box says so itself, and stays.
    EditKvm {
        id: editKvmBox

        objectName: "editKvm"
        editing: window.editing
        actions: window.actions
    }
    // What can be done to a machine, and the question before what cannot be taken back.
    MachineMenu {
        id: machineMenu

        objectName: "machineMenu"
        actions: window.actions
    }
    // The two boxes a menu leads to: keys of one's own choosing, and a text to type.
    SendKeys {
        id: keysBox

        objectName: "sendKeys"
        actions: window.actions
    }
    TypeText {
        id: textBox

        objectName: "typeText"
        actions: window.actions
    }
    // How one thing done to several machines is going.
    BulkProgress {
        id: runBox

        objectName: "bulkProgress"
        actions: window.actions
    }
    // After them, and so over them: what was put into a box is asked about once more.
    // A machine's virtual media through its qKVM: an admin's, opened from its menu.
    MediaBox {
        id: mediaBox

        objectName: "mediaBox"
        actions: window.actions
    }
    Question {
        id: questionBox

        objectName: "question"
        actions: window.actions
    }
    // What was done, and what is under way: in the corner, over everything.
    NoticeList {
        objectName: "notices"
        notices: window.actions.notices
        overTheWall: window.walled && !window.enlarged && !window.boxOpen
    }
}
