import QtQuick
import QtQuick.Layouts
import QtQuick.Templates as T
import Qkvm

// Machine identity and navigation keep their place while the input state changes.
// More moves the same controls into a popup; the keyboard floats over the picture.
// The header is the page's (index.html, "focus-bar"): who the machine is at the left, its tools
// packed at the right beside Close, with their words where the row has room for them (More takes
// the least needed first), and Control's words saying where its keyboard and mouse are (app.js,
// the label of "focus-control"). What went wrong, or is to be known while they are taken, has one
// line of its own in the bar, always there, so that the picture never moves for it: cut at its
// end where it is long, and whole in its hint and in Details.
Item {
    id: enlarged
    required property WallView view
    required property ActionsView actions
    required property ControlView control
    property bool old: false
    property bool videoDetailsOpen: false
    property bool keyboardOpen: false
    property bool externalPanel: false
    property bool alwaysOnTop: false
    property bool canPin: false
    property bool fullScreen: false
    signal pinAsked
    signal fullScreenAsked
    readonly property var video: view.liveDiagnostics
    readonly property string videoDecoder: video.decoding === "gpu" ? Words.videoDecoderGpu : video.decoding === "software" ? Words.videoDecoderSoftware : Words.videoUnknown
    readonly property string videoSize: video.width > 0 && video.height > 0 ? Words.videoResolution.arg(video.width).arg(video.height) : Words.videoUnknown
    readonly property var card: view.enlargedCard
    readonly property string status: card.status ?? ""
    // How large the picture is (b10): the scale chosen for this machine (0: fitted), the machine's own
    // pixels, whether it is shown at that scale, and where it lies in the view's content then: in the
    // middle where it is smaller. The machine's pixels are those of the pictures the engine draws, as
    // the fitted view and the page go by them: while it is live, as its pictures come (the size said
    // with them, else the size they were decoded at), and followed at once when they change; only
    // without a live picture as its card says (the status's video size, which the stills are of).
    readonly property size livePixels: control.pictureSize.width > 0 && control.pictureSize.height > 0 ? control.pictureSize : Qt.size(video.width ?? 0, video.height ?? 0)
    readonly property bool liveSized: view.liveStream !== 0 && livePixels.width > 0 && livePixels.height > 0
    readonly property real scaleWanted: view.scaleOf(view.enlargedScale)
    readonly property size machinePixels: liveSized ? livePixels : (card.pictureSize?.width ?? 0) > 0 ? card.pictureSize : Qt.size(video.width ?? 0, video.height ?? 0)
    readonly property bool showsScreen: picture.stream !== 0
    readonly property bool scaled: scaleWanted > 0 && showsScreen && machinePixels.width > 0 && machinePixels.height > 0
    readonly property rect lies: view.pictureIn(Qt.size(screenView.width, screenView.height), machinePixels, scaled ? scaleWanted : 0, Screen.devicePixelRatio, Qt.point(0, 0))
    // What was in the middle of the view before another size was chosen, each way 0 to 1 over its
    // content: put in the middle again once the content has its new size (the page's scaledToSize).
    property point middle: Qt.point(0.5, 0.5)
    // How wide the room of each scroll bar is, beside the picture at its right and under it: while the
    // picture is at a scale, the view is that much smaller each way, bars or none, so that they never
    // lie over what the pointer does on the machine's screen, and do not come and go under the pointer
    // (the page's scroll bars take room of their own as well).
    readonly property real scrollRoom: 12

    function chooseScale(choice) {
        const was = screenView;
        middle = was.contentWidth > was.width || was.contentHeight > was.height ? Qt.point((was.contentX + was.width / 2) / was.contentWidth, (was.contentY + was.height / 2) / was.contentHeight) : Qt.point(0.5, 0.5);
        view.chooseScale(choice);
        Qt.callLater(keepTheMiddle);
    }
    function keepTheMiddle() {
        screenView.contentX = Math.max(0, Math.min(middle.x * screenView.contentWidth - screenView.width / 2, screenView.contentWidth - screenView.width));
        screenView.contentY = Math.max(0, Math.min(middle.y * screenView.contentHeight - screenView.height / 2, screenView.contentHeight - screenView.height));
    }
    // Where the pointer is in the machine's picture: fitted, by its shape in the view; at a scale, by
    // where it lies in the view now, as it is scrolled.
    function pointerAt(x, y) {
        if (scaled)
            control.pointerOver(x, y, picture.x - screenView.contentX, picture.y - screenView.contentY, picture.width, picture.height);
        else
            control.pointerAt(x, y, pointer.width, pointer.height);
    }
    readonly property bool controlled: control.on
    readonly property bool screenshotHoldsWindow: capture.holdsWindow
    readonly property bool localKeys: !controlled || control.localInput
    // The keyboard on the screen or the details are open: the window's letters are theirs then,
    // as the page's are inside its screen keyboard and its popovers (Main.qml's F).
    readonly property bool panelOpen: keyboardOpen || videoDetailsOpen
    readonly property string wayBackWhole: Words.givesThemBack.arg(control.leaveKeyName)
    readonly property string wayBackShort: Words.givesThemBackShort.arg(control.leaveKeyName)
    // Who holds the machine's keyboard and mouse, as Control's words name them: cut where the
    // name is longer than 80 px, so that Control, Actions, More and Close have their place in the
    // narrowest window's row whatever the name.
    readonly property string holder: buttonLetters.elidedText(control.ownerName, Qt.ElideRight, 80)
    // Asked for, and the machine has not answered: Control says Asking..., and a click again, or C,
    // stops it.
    readonly property bool asking: control.asking && !controlled
    // Control's place: one width for all it says for whoever has the keyboard and mouse now (Control,
    // Asking..., and where somebody else has them, Take over from them and Controlled by them), the
    // widest of those (its symbol, 14 px, the 6 between and 12 on each side around the words), and
    // never less than 112 px. Nothing beside it moves when its words change, and a second click lands
    // on it. (Pressed, while they are taken here, it is as wide.)
    readonly property real slotForTheHolder: {
        const said = [Words.control, Words.asking];
        if (control.ownerName !== "" && !control.ownerInAnotherWindow)
            said.push(Words.takeOverFrom.arg(holder), Words.controlledBy.arg(holder));
        return Math.max(112, Math.ceil(Math.max(...said.map(words => buttonLetters.advanceWidth(words)))) + 14 + 6 + 24);
    }
    // And it does not narrow again while this machine is shown large: the machine forgets who held
    // them when they come here, and the click that gives them back would land on Next. The widest it
    // was for this machine; another machine shown large starts it anew.
    property real widestSlot: 0
    readonly property real controlSlot: Math.max(slotForTheHolder, widestSlot)
    onSlotForTheHolderChanged: widestSlot = Math.max(widestSlot, slotForTheHolder)

    FontMetrics {
        id: buttonLetters

        font.family: Application.font.family
        font.pixelSize: Theme.textSize
        font.weight: Font.Medium
    }

    function settleInput() {
        if (!externalPanel && !headerTools.opened && !tools.opened && !scalePick.opened && !keyboardOpen && !videoDetailsOpen && !keysQuestion.visible) {
            control.setLocalInput(false);
            forceActiveFocus();
        }
    }
    function localPanel() {
        control.wentOn();
        control.setLocalInput(true);
    }
    function closePanels() {
        keyboardOpen = false;
        videoDetailsOpen = false;
        headerTools.closeMenu();
        tools.closeMenu();
        keyboard.modifiers = [];
        keysQuestion.close();
    }
    function showKeyboard() {
        if (!control.possible)
            return;
        keyboardOpen = !keyboardOpen;
        if (keyboardOpen) {
            localPanel();
            keyboard.forceActiveFocus();
        } else {
            settleInput();
        }
    }
    // Ctrl+Alt+Del or Alt+F4 is asked for: asked first (keysQuestion), for the machine it is. From
    // the keyboard on the screen, with the chord as it was pressed there.
    function askKeys(keys, chord) {
        if (!controlled)
            return;
        keysQuestion.keys = keys;
        keysQuestion.chord = chord ?? [];
        keysQuestion.machine = view.enlarged;
        localPanel();
        keysQuestion.open();
    }
    onControlledChanged: {
        if (!controlled)
            closePanels();
    }
    onVideoDetailsOpenChanged: {
        view.details.opened = videoDetailsOpen;
        if (videoDetailsOpen)
            localPanel();
        else
            settleInput();
    }
    onExternalPanelChanged: {
        if (externalPanel)
            localPanel();
        else
            settleInput();
    }
    Keys.onShortcutOverride: event => {
        // (C while the machine is asked stops the asking, as the button does, and so does Space on
        // the button, which says Asking... then: neither is a key pressed before the machine
        // answered, which would end the asking by itself, said otherwise, and leave them nothing.)
        const stops = event.key === Qt.Key_C && event.modifiers === Qt.NoModifier || take.activeFocus && (event.key === Qt.Key_Space || event.key === Qt.Key_Select);
        if (!event.isAutoRepeat && !(stops && enlarged.asking))
            control.wentOn(true);
        event.accepted = controlled && !control.localInput;
    }
    Keys.onPressed: event => {
        if (!event.isAutoRepeat)
            control.wentOn(true);
        event.accepted = controlled && !control.localInput;
    }
    Keys.onReleased: event => event.accepted = controlled && !control.localInput
    Keys.onEscapePressed: {
        if (keyboardOpen) {
            keyboardOpen = false;
            settleInput();
        } else if (videoDetailsOpen) {
            videoDetailsOpen = false;
        } else if (!controlled) {
            view.closeEnlarged();
        }
    }
    // C does what the Control button does, as the page's C does: takes keyboard and mouse, asks to
    // take them over, or stops asking. Not while they are taken (every key is the machine's then),
    // nor while the keyboard on the screen or the details are open (its key is theirs, as on the
    // page); where something is typed it is a letter, and while a box or a menu is open the box's.
    Shortcut {
        sequence: "C"
        // (Held, it does it once: taken, and then not given back by its repeat.)
        autoRepeat: false
        enabled: !enlarged.panelOpen && take.offered && !take.resting
        onActivated: enlarged.asking ? enlarged.control.giveBack() : enlarged.control.take()
    }
    Keys.onLeftPressed: if (!controlled)
        view.stepEnlarged(-1)
    Keys.onRightPressed: if (!controlled)
        view.stepEnlarged(1)
    Component.onCompleted: {
        widestSlot = slotForTheHolder;
        forceActiveFocus();
    }
    Connections {
        target: enlarged.control
        function onChanged() {
            if (enlarged.control.on && (enlarged.externalPanel || enlarged.keyboardOpen || enlarged.videoDetailsOpen || headerTools.opened || tools.opened || keysQuestion.visible) && !enlarged.control.localInput)
                enlarged.control.setLocalInput(true);
            else if (enlarged.control.on && !enlarged.control.localInput)
                enlarged.forceActiveFocus();
        }
    }
    Connections {
        target: enlarged.view
        function onEnlargedChanged() {
            enlarged.closePanels();
            enlarged.widestSlot = enlarged.slotForTheHolder;
            // (Another machine: from its top left corner.)
            screenView.contentX = 0;
            screenView.contentY = 0;
        }
    }
    // How wide the picture is drawn, fitted, in the monitor's own pixels. (At a scale the whole of the
    // machine's picture is asked for: MergedWall.)
    Binding {
        target: enlarged.view
        property: "enlargedPixels"
        value: Math.max(0, Math.round(screenView.width * Screen.devicePixelRatio))
    }

    // The line under the tools: one line in the colour of a warning, cut at its end where it is
    // long. Whole a moment after the pointer came to rest on it where it is cut, and a click asks
    // for Details, which say it whole too.
    component LineUnderTools: Text {
        id: line

        signal wholeAsked

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        height: 15
        textFormat: Text.PlainText
        color: Theme.warn
        font.pixelSize: Theme.headingSize
        verticalAlignment: Text.AlignVCenter
        maximumLineCount: 1
        elide: Text.ElideRight
        Accessible.name: text

        HoverHandler {
            id: resting

            onHoveredChanged: {
                if (hovered) {
                    waiting.restart();
                } else {
                    waiting.stop();
                    whole.close();
                }
            }
        }
        TapHandler {
            onTapped: line.wholeAsked()
        }
        Timer {
            id: waiting

            interval: 600
            onTriggered: {
                if (resting.hovered && line.truncated)
                    whole.open();
            }
        }
        Tip {
            id: whole

            objectName: line.objectName + "Whole"
            text: line.text
        }
    }

    Rectangle {
        id: bar
        objectName: "consoleHeader"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        // (The line under the tools is in it always: the picture stays where it is.)
        height: header.height + tools.height + 16
        color: Theme.panel
        RowLayout {
            id: header
            objectName: "consoleHeaderRow"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            height: Theme.controlHeight + 16
            spacing: 10
            // Who the machine is: all the row leaves the tools, and never less than the page's
            // (app.css, ".focus-identity": clamp(90px, 22vw, 210px)).
            Item {
                id: identity

                readonly property real least: Math.max(90, Math.min(210, enlarged.width * 0.22))

                Layout.fillWidth: true
                Layout.minimumWidth: least
                Layout.fillHeight: true
                Rectangle {
                    objectName: "consoleMachineState"
                    width: 8
                    height: 8
                    radius: 4
                    anchors.verticalCenter: machineName.verticalCenter
                    color: Theme.stateColor(enlarged.status)
                    Accessible.name: enlarged.card.label ?? ""
                }
                Text {
                    id: machineName
                    objectName: "enlargedName"
                    anchors.left: parent.left
                    anchors.leftMargin: 18
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: -8
                    textFormat: Text.PlainText
                    text: enlarged.card.name ?? ""
                    color: Theme.text
                    font.pixelSize: Theme.titleSize
                    font.weight: Font.DemiBold
                    elide: Text.ElideMiddle
                    Accessible.name: text
                }
                Text {
                    objectName: "consoleMachineDetail"
                    anchors.left: machineName.left
                    anchors.right: parent.right
                    anchors.top: machineName.bottom
                    anchors.topMargin: 2
                    textFormat: Text.PlainText
                    // (Its large view's words where it has its own: a PC with Intel AMT says that it is live here.)
                    text: [enlarged.card.label ?? "", enlarged.card.host ?? "", (enlarged.card.largeMeta ?? "") !== "" ? enlarged.card.largeMeta : enlarged.card.meta ?? ""].filter(part => part !== "").join(" · ")
                    color: Theme.muted
                    font.pixelSize: Theme.smallSize
                    elide: Text.ElideRight
                    Accessible.name: text
                }
            }
            // The tools: as wide as they are, all of them, where the row has the room; otherwise
            // what it leaves the machine's name, and More takes what does not fit.
            ConsoleOverflow {
                id: headerTools
                Layout.preferredWidth: Math.min(full, header.width - identity.least - header.spacing)
                packed: true
                menuName: "consoleMore"
                localKeys: enlarged.localKeys
                // (In the page's order: index.html, "focus-bar".)
                items: [previous, next, take, giveBack, actionsButton, consoleButton, fullscreen, closeButton]
                onInteractionStarted: enlarged.localPanel()
                // (A turn later, as for the keys' row below.)
                onInteractionEnded: Qt.callLater(enlarged.settleInput)
                ConsoleAction {
                    id: previous
                    objectName: "enlargedPrevious"
                    symbol: "chevronLeft"
                    hint: Words.previousKvm
                    priority: 20
                    focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                    onClicked: enlarged.view.stepEnlarged(-1)
                }
                ConsoleAction {
                    id: next
                    objectName: "enlargedNext"
                    symbol: "chevronRight"
                    hint: Words.nextKvm
                    priority: 20
                    focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                    onClicked: enlarged.view.stepEnlarged(1)
                }
                // Control: one button, as the page's one, whose words say where the keyboard and mouse
                // are (who has them, where somebody else does; Asking... while they are asked for),
                // and whose click asks for them, or stops the asking. The keys stay on it through all
                // of that. (Pressed, while they are taken here, it is the one below.)
                ConsoleAction {
                    id: take
                    objectName: "enlargedControl"
                    offered: !enlarged.controlled && (enlarged.control.possible || enlarged.control.asking)
                    // (While the machine owes a word, it asks nothing, and says so by its look. It is
                    // not made a button that cannot be pressed: that would lose the keys, and the
                    // console's own keys with them, at Space on Asking..., which makes it owe one.)
                    resting: !enlarged.asking && enlarged.control.busy
                    implicitWidth: enlarged.controlSlot
                    pinned: true
                    symbol: "command"
                    text: enlarged.asking ? Words.asking : enlarged.control.takeoverPossible ? Words.takeOverFrom.arg(enlarged.holder) : !enlarged.control.ownerInAnotherWindow && enlarged.control.ownerName !== "" ? Words.controlledBy.arg(enlarged.holder) : Words.control
                    // (Whoever has them, whole, where the words cut the name.)
                    hint: enlarged.asking ? Words.stopAskingFor.arg(enlarged.card.name ?? "") : enlarged.control.takeoverPossible ? Words.takeOverFromAfterConfirmation.arg(enlarged.control.ownerName) : !enlarged.control.ownerInAnotherWindow && enlarged.control.ownerName !== "" ? Words.controlledBy.arg(enlarged.control.ownerName) : Words.takeKeyboardAndMouseOf.arg(enlarged.card.name ?? "")
                    onClicked: {
                        if (resting)
                            return;
                        if (enlarged.asking)
                            enlarged.control.giveBack();
                        else
                            enlarged.control.take();
                    }
                }
                ConsoleAction {
                    id: giveBack
                    objectName: "controlGiveBack"
                    offered: enlarged.controlled
                    implicitWidth: enlarged.controlSlot
                    pinned: true
                    primary: true
                    focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                    symbol: "command"
                    text: Words.control
                    hint: Words.giveBackToThisPc
                    checked: true
                    onClicked: enlarged.control.giveBack()
                }
                ConsoleAction {
                    id: actionsButton
                    objectName: "enlargedActions"
                    offered: enlarged.actions.allowed
                    pinned: true
                    symbol: "zap"
                    text: Words.actions
                    hint: Words.actionsFor.arg(enlarged.card.name ?? "")
                    focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                    onClicked: enlarged.actions.open(enlarged.view.enlarged)
                }
                ConsoleAction {
                    id: consoleButton
                    objectName: "enlargedConsole"
                    offered: enlarged.card.openable === true
                    symbol: "external"
                    text: Words.openConsole
                    hint: Words.openTheConsoleOf.arg(enlarged.card.name ?? "")
                    priority: 40
                    focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                    onClicked: enlarged.actions.openConsole(enlarged.view.enlarged)
                }
                ConsoleAction {
                    id: fullscreen
                    objectName: "enlargedFullscreen"
                    symbol: enlarged.fullScreen ? "minimize" : "maximize"
                    // (Its words as the page's, whichever way it goes; its symbol and its hint say which.)
                    text: Words.fullscreen
                    hint: enlarged.fullScreen ? Words.leaveFullscreen : Words.fullscreen
                    priority: 30
                    focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                    onClicked: enlarged.fullScreenAsked()
                }
                ConsoleAction {
                    id: closeButton
                    objectName: "enlargedClose"
                    pinned: true
                    last: true
                    symbol: "x"
                    hint: Words.close
                    focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                    onClicked: enlarged.view.closeEnlarged()
                }
            }
        }
        ConsoleOverflow {
            id: tools
            objectName: "consoleTools"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: header.bottom
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            menuName: "inputMore"
            description: qsTr("More input controls")
            localKeys: enlarged.localKeys
            items: [keyboardButton, cad, win, altTab, altF4, printScreen, typeText, save, copy, favorite, relative, pin]
            onInteractionStarted: enlarged.localPanel()
            // (A turn later: a key of the menu that is asked first, Ctrl+Alt+Del or Alt+F4, opens
            // its question before that, and the keys are not given back to the machine between.)
            onInteractionEnded: Qt.callLater(enlarged.settleInput)
            ConsoleAction {
                id: keyboardButton
                objectName: "controlKeyboard"
                offered: enlarged.control.possible
                text: qsTr("Keyboard")
                checked: enlarged.keyboardOpen
                priority: 100
                focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                onClicked: enlarged.showKeyboard()
            }
            ConsoleAction {
                id: cad
                objectName: "controlCtrlAltDel"
                offered: enlarged.controlled
                text: Words.ctrlAltDel
                priority: 70
                focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                onClicked: enlarged.askKeys("ctrlAltDel")
            }
            ConsoleAction {
                id: win
                objectName: "controlWin"
                offered: enlarged.controlled
                text: "Win"
                priority: 20
                focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                onClicked: enlarged.control.tapKeys([227])
            }
            ConsoleAction {
                id: altTab
                objectName: "controlAltTab"
                offered: enlarged.controlled
                text: "Alt+Tab"
                priority: 40
                focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                onClicked: enlarged.control.tapKeys([226, 43])
            }
            ConsoleAction {
                id: altF4
                objectName: "controlAltF4"
                offered: enlarged.controlled
                text: Words.altF4
                priority: 10
                focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                onClicked: enlarged.askKeys("altF4")
            }
            ConsoleAction {
                id: printScreen
                objectName: "controlPrintScreen"
                offered: enlarged.controlled
                text: "PrtSc"
                priority: 10
                focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                onClicked: enlarged.control.tapKeys([70])
            }
            ConsoleAction {
                id: typeText
                objectName: "controlTypeText"
                offered: enlarged.controlled && enlarged.control.textAllowed
                text: Words.typeText
                priority: 90
                focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                onClicked: enlarged.actions.openText(enlarged.view.enlarged)
            }
            ConsoleAction {
                id: save
                objectName: "saveScreenshot"
                offered: !enlarged.controlled
                text: Words.saveScreenshot
                enabled: capture.possible
                priority: 80
                onClicked: capture.save()
            }
            ConsoleAction {
                id: copy
                objectName: "copyScreenshot"
                offered: !enlarged.controlled
                text: Words.copyScreenshot
                enabled: capture.possible
                priority: 70
                onClicked: capture.copy()
            }
            ConsoleAction {
                id: favorite
                objectName: "enlargedFavorite"
                offered: !enlarged.controlled && enlarged.view.shortcutsAvailable
                symbol: "star"
                text: enlarged.view.favoriteKeys.includes(enlarged.view.enlarged) ? qsTr("Remove favorite") : qsTr("Add favorite")
                priority: 10
                onClicked: enlarged.view.toggleFavorite(enlarged.view.enlarged)
            }
            ConsoleAction {
                id: relative
                objectName: "relativeMouse"
                text: Words.relativeMouse
                checked: enlarged.control.relative
                enabled: !enlarged.controlled && enlarged.control.possible && !enlarged.control.busy && enlarged.control.mouseSelectable
                hint: enlarged.controlled ? qsTr("Release control before changing the mouse mode") : Words.relativeMouseHelp.arg(enlarged.control.leaveKeyName)
                priority: 60
                onClicked: enlarged.control.setRelative(!enlarged.control.relative)
            }
            ConsoleAction {
                id: pin
                objectName: "alwaysOnTop"
                offered: !enlarged.controlled
                text: Words.alwaysOnTop
                checked: enlarged.alwaysOnTop
                enabled: enlarged.canPin
                priority: 0
                onClicked: enlarged.pinAsked()
            }
        }
        // One line under the tools, in the room the bar has there always: why keyboard and mouse
        // were not given, while they are not taken; what is to be known, while they are.
        LineUnderTools {
            id: problem
            objectName: "controlProblem"
            anchors.top: tools.bottom
            visible: !enlarged.controlled && enlarged.control.problem !== ""
            text: enlarged.control.problem
            onWholeAsked: enlarged.videoDetailsOpen = true
        }
        LineUnderTools {
            id: note
            objectName: "controlNote"
            anchors.top: tools.bottom
            visible: enlarged.controlled && (enlarged.control.relative || enlarged.control.note !== "")
            text: [enlarged.control.relative ? (enlarged.control.localInput ? qsTr("Relative mouse is paused while local controls are open.") : Words.relativeMouseActive) : "", enlarged.control.note].filter(part => part !== "").join(" ")
            onWholeAsked: enlarged.videoDetailsOpen = true
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.lineStrong
        }
    }

    Rectangle {
        id: statusBar
        objectName: "consoleStatus"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 28
        color: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 10
            Text {
                objectName: "enlargedLive"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                textFormat: Text.PlainText
                text: {
                    const v = enlarged.video;
                    // (A PC with Intel AMT in health only has neither: no KVM session is opened.)
                    if (!enlarged.view.live)
                        return enlarged.card.amtView === "health" ? qsTr("No live picture: health only") : qsTr("No live picture: stills");
                    if (!v.hasPicture)
                        return v.connected ? qsTr("Live · waiting for the first picture") : qsTr("Live · connecting...");
                    let said = qsTr("Live · %1 %2").arg(v.codec === "h264" ? "H.264" : "JPEG").arg(enlarged.videoSize);
                    // What reaches the screen, where that is known; otherwise what arrives.
                    if (v.engineKnown && v.shownRateKnown)
                        said += qsTr(" · %1/s").arg(Number(v.shownFps).toFixed(1));
                    else if (v.rateKnown)
                        said += qsTr(" · %1/s").arg(Number(v.receivedFps).toFixed(1));
                    if (v.engineKnown)
                        said += qsTr(" · %1 dropped").arg(v.dropped);
                    return said;
                }
                color: Theme.muted
                font.pixelSize: Theme.smallSize
                elide: Text.ElideRight
                Accessible.name: text
            }
            // How large the picture is, as the page's pick under it: Fit, or a scale of the machine's own
            // pixels. In a place of its own whether or not it is offered (a machine without a picture has
            // none): nothing beside it moves for it. No key of its own, as on the page.
            Item {
                Layout.preferredWidth: 104
                Layout.preferredHeight: 24
                Choice {
                    id: scalePick

                    objectName: "enlargedScale"
                    anchors.fill: parent
                    visible: enlarged.showsScreen
                    symbol: "zoomIn"
                    hint: qsTr("How large the picture is: fitted into the window, or so many of this screen's pixels to each of the machine's (100 %: one to one)")
                    focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                    choices: [
                        {
                            value: "fit",
                            label: qsTr("Fit")
                        },
                        {
                            value: "50",
                            label: "50 %"
                        },
                        {
                            value: "75",
                            label: "75 %"
                        },
                        {
                            value: "100",
                            label: "100 %"
                        },
                        {
                            value: "125",
                            label: "125 %"
                        },
                        {
                            value: "140",
                            label: "140 %"
                        },
                        {
                            value: "200",
                            label: "200 %"
                        }
                    ]
                    value: enlarged.view.enlargedScale
                    onPicked: value => enlarged.chooseScale(value)
                    onOpenedChanged: {
                        if (opened)
                            enlarged.localPanel();
                        else
                            Qt.callLater(enlarged.settleInput);
                    }
                }
            }
            Text {
                objectName: "controlAsking"
                visible: enlarged.control.asking
                text: Words.asking
                textFormat: Text.PlainText
                color: Theme.accentText
                font.pixelSize: Theme.smallSize
            }
            Text {
                objectName: "controlOwner"
                visible: !enlarged.controlled && (enlarged.control.ownerInAnotherWindow || enlarged.control.ownerName !== "")
                Layout.maximumWidth: enlarged.width * 0.35
                textFormat: Text.PlainText
                text: enlarged.control.ownerInAnotherWindow ? qsTr("Controlled in another window") : qsTr("Controlled by %1 (last reported)").arg(enlarged.control.ownerName)
                color: Theme.warn
                font.pixelSize: Theme.smallSize
                elide: Text.ElideRight
                Accessible.name: text
            }
            Text {
                objectName: "controlGoesTo"
                visible: enlarged.controlled
                text: qsTr("Controlling")
                textFormat: Text.PlainText
                color: Theme.accentText
                font.pixelSize: Theme.smallSize
            }
            Text {
                objectName: "controlViewing"
                visible: !enlarged.controlled && !enlarged.control.asking && !enlarged.control.ownerInAnotherWindow && enlarged.control.ownerName === ""
                text: qsTr("Viewing")
                textFormat: Text.PlainText
                color: Theme.muted
                font.pixelSize: Theme.smallSize
            }
            Text {
                objectName: "controlWayBack"
                visible: enlarged.controlled
                textFormat: Text.PlainText
                text: enlarged.control.localInput ? qsTr("Click picture to resume") : enlarged.wayBackShort
                color: Theme.muted
                font.pixelSize: Theme.smallSize
            }
            Button {
                id: videoDetailsButton
                objectName: "enlargedVideoDetails"
                implicitHeight: 24
                text: qsTr("Details")
                hint: Words.videoDetails
                checked: enlarged.videoDetailsOpen
                focusPolicy: enlarged.localKeys ? Qt.StrongFocus : Qt.NoFocus
                onClicked: enlarged.videoDetailsOpen = !enlarged.videoDetailsOpen
            }
        }
    }

    Dialog {
        id: takeover
        objectName: "controlTakeoverQuestion"
        title: qsTr("Take over keyboard and mouse")
        width: Math.min(460, parent ? parent.width - 24 : 460)
        readonly property bool pending: enlarged.control.takeoverPending
        onPendingChanged: pending ? open() : close()
        onOpened: takeoverNo.forceActiveFocus()
        onClosed: {
            if (pending)
                enlarged.control.answerTakeover(false);
        }
        Column {
            width: takeover.availableWidth
            spacing: 14
            Flickable {
                width: parent.width
                height: Math.min(takeoverText.implicitHeight, Math.max(60, (takeover.parent ? takeover.parent.height : 400) - 180))
                contentHeight: takeoverText.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Text {
                    id: takeoverText
                    objectName: "controlTakeoverText"
                    textFormat: Text.PlainText
                    width: parent.width
                    text: qsTr("%1 has the keyboard and mouse of %2. Take them over? What they hold down is let go of first, and they are told that you took over.").arg(enlarged.control.takeoverOwner).arg(enlarged.card.name ?? "")
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    wrapMode: Text.Wrap
                }
            }
            Row {
                anchors.right: parent.right
                spacing: 8
                Button {
                    id: takeoverNo
                    objectName: "controlTakeoverNo"
                    text: Words.cancel
                    onClicked: enlarged.control.answerTakeover(false)
                }
                Button {
                    objectName: "controlTakeoverYes"
                    text: qsTr("Take over")
                    primary: true
                    enabled: takeover.pending && enlarged.control.takeoverPossible
                    onClicked: enlarged.control.answerTakeover(true)
                }
            }
        }
    }

    // The picture's view: the machine's screen fitted into it, or at a scale of its own pixels (b10), in
    // the middle where it is smaller and scrolled where it is larger, as the page's focus-scroll. A drag
    // or the wheel moves it only while it is watched; while keyboard and mouse are taken they are the
    // machine's, and its scroll bars move it.
    Flickable {
        id: screenView

        objectName: "enlargedView"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: enlarged.scaled ? enlarged.scrollRoom : 0
        anchors.top: bar.bottom
        anchors.bottom: statusBar.top
        anchors.bottomMargin: enlarged.scaled ? enlarged.scrollRoom : 0
        clip: true
        contentWidth: enlarged.scaled ? Math.max(width, enlarged.lies.width) : width
        contentHeight: enlarged.scaled ? Math.max(height, enlarged.lies.height) : height
        readonly property bool larger: contentWidth > width + 0.5 || contentHeight > height + 0.5
        interactive: enlarged.scaled && larger && !enlarged.controlled
        boundsBehavior: Flickable.StopAtBounds
        // (Where it was let go, it stays, or as good as: it is not thrown on.)
        flickDeceleration: 20000

        CardScreen {
            id: picture

            objectName: "enlargedScreen"
            x: enlarged.scaled ? enlarged.lies.x : 0
            y: enlarged.scaled ? enlarged.lies.y : 0
            width: enlarged.scaled ? enlarged.lies.width : screenView.width
            height: enlarged.scaled ? enlarged.lies.height : screenView.height
            clipTo: screenView
            // Its live pictures where they come, and its snapshots where they do not.
            stream: enlarged.view.liveStream !== 0 ? enlarged.view.liveStream : (enlarged.card.stream ?? 0)
            status: enlarged.status
            title: enlarged.card.title ?? ""
            text: enlarged.card.text ?? ""
            action: enlarged.card.action ?? ""
            ports: enlarged.card.ports ?? []
            // (A server whose screen comes live is shown as a KVM is: its readout would lie over it.)
            server: enlarged.view.liveStream !== 0 ? ({}) : (enlarged.card.server ?? ({}))
            old: enlarged.old
            onPortAsked: port => enlarged.actions.showPort(enlarged.view.enlarged, port)
            onActionAsked: enlarged.actions.cardAction(enlarged.view.enlarged)
        }
    }
    ScreenCapture {
        id: capture

        objectName: "screenCapture"
        anchors.fill: screenView
        view: enlarged.view
        actions: enlarged.actions
        control: enlarged.control
    }
    RelativePointer {
        objectName: "relativePointer"
        anchors.fill: screenView
        control: enlarged.control
        // (At a scale, where the picture lies in the view now: the pointer is held to what of it is seen.)
        placed: enlarged.scaled ? Qt.rect(picture.x - screenView.contentX, picture.y - screenView.contentY, picture.width, picture.height) : Qt.rect(0, 0, 0, 0)
    }
    // While keyboard and mouse are taken: what the pointer does over the picture is the machine's.
    // This PC's pointer stays in sight; the machine draws its own. (Where in the picture a place
    // of the view is, the view works out: fitted, the picture keeps its shape and has bars beside it;
    // at a scale, it lies where it is scrolled to: pointerAt.)
    MouseArea {
        id: pointer

        objectName: "controlPointer"
        anchors.fill: screenView
        enabled: enlarged.controlled
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        cursorShape: enlarged.control.pointerCaptured ? Qt.BlankCursor : Qt.ArrowCursor
        onPositionChanged: mouse => enlarged.pointerAt(mouse.x, mouse.y)
        onExited: enlarged.control.pointerAt(-1, -1, pointer.width, pointer.height)
        onPressed: mouse => {
            if (enlarged.control.localInput) {
                enlarged.closePanels();
                enlarged.control.setLocalInput(false);
                enlarged.forceActiveFocus();
                return;
            }
            enlarged.pointerAt(mouse.x, mouse.y);
            enlarged.control.button(mouse.button, true);
        }
        onReleased: mouse => {
            enlarged.pointerAt(mouse.x, mouse.y);
            enlarged.control.button(mouse.button, false);
        }
        onWheel: wheel => enlarged.control.wheel(wheel.angleDelta.x, wheel.angleDelta.y)
    }
    // A picture larger than the view has scroll bars, always, in their room beside it (scrollRoom): they
    // move it while keyboard and mouse are the machine's as well, and take nothing of the machine's screen.
    component ScreenScroll: T.ScrollBar {
        id: scroll

        required property Flickable target

        // (What the bar moved goes to the view; the view's own place is what the bar shows.)
        onPositionChanged: {
            if (!scroll.pressed)
                return;
            if (scroll.orientation === Qt.Horizontal)
                scroll.target.contentX = scroll.position * scroll.target.contentWidth;
            else
                scroll.target.contentY = scroll.position * scroll.target.contentHeight;
        }
        padding: 2
        minimumSize: 0.05
        policy: T.ScrollBar.AlwaysOn
        contentItem: Rectangle {
            implicitWidth: 8
            implicitHeight: 8
            radius: 4
            color: scroll.pressed ? Theme.text : Theme.muted
        }
        background: Rectangle {
            color: Theme.inset
        }
    }
    // The room of the bars, at the picture's right and under it, as long as the picture is at a scale:
    // their track where they are, and empty where the picture fits that way.
    Rectangle {
        objectName: "enlargedScrollRoom"
        visible: enlarged.scaled
        anchors.left: screenView.right
        anchors.right: parent.right
        anchors.top: screenView.top
        anchors.bottom: statusBar.top
        color: Theme.inset
    }
    Rectangle {
        visible: enlarged.scaled
        anchors.left: parent.left
        anchors.right: screenView.right
        anchors.top: screenView.bottom
        anchors.bottom: statusBar.top
        color: Theme.inset
    }
    ScreenScroll {
        objectName: "enlargedScrollX"
        target: screenView
        visible: enlarged.scaled && screenView.contentWidth > screenView.width + 0.5
        orientation: Qt.Horizontal
        anchors.left: screenView.left
        anchors.right: screenView.right
        anchors.top: screenView.bottom
        height: enlarged.scrollRoom
        size: screenView.visibleArea.widthRatio
        position: screenView.visibleArea.xPosition
    }
    ScreenScroll {
        objectName: "enlargedScrollY"
        target: screenView
        visible: enlarged.scaled && screenView.contentHeight > screenView.height + 0.5
        orientation: Qt.Vertical
        anchors.top: screenView.top
        anchors.bottom: screenView.bottom
        anchors.left: screenView.right
        width: enlarged.scrollRoom
        size: screenView.visibleArea.heightRatio
        position: screenView.visibleArea.yPosition
    }
    // And the picture has a frame that says so.
    Rectangle {
        objectName: "controlFrame"
        anchors.fill: screenView
        visible: enlarged.controlled
        color: "transparent"
        border.width: 2
        border.color: Theme.accent
    }
    ConsoleDetails {
        objectName: "videoDiagnostics"
        view: enlarged.view
        anchors.left: screenView.left
        anchors.right: screenView.right
        anchors.bottom: screenView.bottom
        anchors.margins: 12
        height: Math.min(480, Math.max(0, screenView.height - 24))
        visible: enlarged.videoDetailsOpen && height > 0
        onCloseAsked: enlarged.videoDetailsOpen = false
        videoDescription: {
            const video = enlarged.video;
            // (What the line under the tools says, whole, first.)
            const said = problem.visible ? problem.text : note.visible ? note.text : "";
            const lines = (said !== "" ? [said, ""] : []).concat([video.connected === true ? Words.videoConnected : Words.videoWaiting]);
            if (video.transport === "webrtc")
                lines.push(Words.videoWebRtc);
            else if (video.transport === "mjpeg")
                lines.push(Words.videoMjpeg);
            else if (video.transport === "snapshots")
                lines.push(Words.videoSnapshots);
            else if (video.transport === "reconnecting")
                lines.push(Words.videoReconnecting);
            if (video.fallbackReason)
                lines.push(video.fallbackReason);
            if (video.hasPicture === true) {
                lines.push(Words.videoFormat.arg(video.codec === "h264" ? "H.264" : "JPEG").arg(enlarged.videoSize));
                if (video.rateKnown === true)
                    lines.push(Words.videoReceiveRate.arg(Number(video.receivedFps).toFixed(1)).arg(Number(video.receivedMbps).toFixed(2)));
                if (video.engineKnown === true && video.shownRateKnown === true)
                    lines.push(Words.videoShownRate.arg(Number(video.shownFps).toFixed(1)).arg(Number(video.arrivedToShownMs).toFixed(1)));
                lines.push(Words.videoReceiveAge.arg(video.lastReceivedMs));
                lines.push(Words.videoDecoder.arg(enlarged.videoDecoder));
                if (enlarged.view.graphicsCard !== "")
                    lines.push(Words.videoDrawnOn.arg(enlarged.view.graphicsCard));
                if (video.engineKnown === true) {
                    lines.push(Words.videoNotShown.arg(video.replaced).arg(video.dropped).arg(video.refused));
                    lines.push(Words.videoLateSteps.arg(video.lateSteps).arg(video.sourceLateSteps));
                    if (video.decoded > 0 && video.decoding !== "unknown")
                        lines.push(Words.videoDecodeTime.arg(Number(video.decodeMs).toFixed(2)));
                }
            }
            lines.push(Words.videoMeasurementsHow);
            return lines.join("\n");
        }
    }
    // Asked before Ctrl+Alt+Del and Alt+F4 go, as the page asks (app.js, "focus-cad" and
    // "focus-alt-f4"): which keys, on which machine, and for Alt+F4 what it does there. The keys
    // go only to the machine they were asked for, and only while its keyboard and mouse are still
    // taken; Esc, the cross, a click beside it and Cancel send nothing. While it is asked, the keys
    // are this PC's, and what was held is let go of, as for every panel of the screen's own.
    Dialog {
        id: keysQuestion
        objectName: "controlKeysQuestion"
        // "ctrlAltDel" or "altF4", the chord the keyboard on the screen made of it (none from the
        // bar's buttons), and the machine they were asked for.
        property string keys: ""
        property var chord: []
        property string machine: ""
        readonly property bool closesAWindow: keys === "altF4"
        title: closesAWindow ? Words.altF4 : Words.ctrlAltDel
        width: Math.min(460, parent ? parent.width - 24 : 460)
        onOpened: keysNo.forceActiveFocus()
        onClosed: enlarged.settleInput()
        Column {
            width: keysQuestion.availableWidth
            spacing: 14
            Text {
                objectName: "controlKeysText"
                textFormat: Text.PlainText
                width: parent.width
                text: (keysQuestion.closesAWindow ? Words.altF4On : Words.ctrlAltDelOn).arg(enlarged.control.inASentence(enlarged.card.name ?? ""))
                color: Theme.text
                font.pixelSize: Theme.textSize
                wrapMode: Text.Wrap
            }
            Row {
                anchors.right: parent.right
                spacing: 8
                Button {
                    id: keysNo
                    objectName: "controlKeysNo"
                    text: Words.cancel
                    onClicked: keysQuestion.close()
                }
                Button {
                    objectName: "controlKeysYes"
                    text: keysQuestion.title
                    primary: !keysQuestion.closesAWindow
                    danger: keysQuestion.closesAWindow
                    onClicked: {
                        if (enlarged.controlled && enlarged.view.enlarged === keysQuestion.machine) {
                            if (keysQuestion.chord.length > 0)
                                enlarged.control.tapKeys(keysQuestion.chord);
                            else if (keysQuestion.closesAWindow)
                                enlarged.control.tapKeys([226, 61]);
                            else
                                enlarged.control.ctrlAltDel();
                        }
                        keysQuestion.close();
                    }
                }
            }
        }
    }
    FloatingKeyboard {
        id: keyboard
        control: enlarged.control
        visible: enlarged.keyboardOpen
        topLimit: bar.height
        bottomLimit: statusBar.y
        onCloseAsked: {
            enlarged.keyboardOpen = false;
            enlarged.settleInput();
        }
        onKeysAsked: (keys, chord) => enlarged.askKeys(keys, chord)
    }
}
