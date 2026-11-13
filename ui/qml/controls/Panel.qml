// (Not bound to its file, as a delegate is: its parts are used in other files.)
import QtQuick
import QtQuick.Templates as T
import Qkvm

// What drops from a button of the bar as the page's ".popup" (app.css): 264 px wide (or as wide
// as a row with its key needs: widest) under the button's right end, 14 px inside its frame, its
// parts 12 px apart, top to bottom. The parts are Panel.Heading (a small heading over a choice, as
// ".pop-row > span"), Panel.Divider (the page's "hr", from one edge to the other) and Panel.Row
// (the page's ".menu-row"), and whatever else is put in it. A row that leads somewhere closes it
// first (pick), as the page's do (app.js, drop); Esc and a click beside it close it, and so does a
// click on its button, which opened it. It is as high as its parts, or as the window leaves under
// its button: then it scrolls, a mark at its right edge says how far, and a part the keys come to
// is brought into sight. Up and Down go from one part to the next, round the ends; Tab does too.
// Opened from the keyboard, it shows where the keys are, as its button did, and its button shows it
// again once it has closed.
T.Popup {
    id: panel

    default property alias parts: rows.data
    // What scrolls in it.
    readonly property Flickable flick: scroller
    // What the keys are at when it opens.
    property Item first: null
    // How high the window lets it be: looked at each time it opens.
    property real room: 100000
    // How wide its parts need it to be inside its frame: the page's 236, or what its widest row needs.
    property real widest: 236
    // It was opened from the keyboard: its button had the keys, and showed it.
    property bool byKeys: false

    signal picked(string kind, var value)

    // Closes it, then tells what was chosen.
    function pick(kind: string, value: var) {
        panel.close();
        panel.picked(kind, value);
    }
    // Opens it, or closes it where it is open: its button's click.
    function toggle() {
        if (panel.opened)
            panel.close();
        else
            panel.open();
    }
    // The parts the keys go to, in their order.
    function stops() {
        const found = [];
        const walk = under => {
            for (let at = 0; at < under.children.length; ++at) {
                const one = under.children[at];
                if (!one.visible || !one.enabled)
                    continue;
                if (one.activeFocusOnTab)
                    found.push(one);
                else
                    walk(one);
            }
        };
        walk(rows);
        return found;
    }
    // The keys go on to the part before or after the one they are at.
    function step(by: int) {
        const all = stops();
        if (all.length === 0)
            return;
        const at = all.findIndex(one => one.activeFocus);
        all[((at < 0 ? (by > 0 ? -1 : 0) : at) + by + all.length) % all.length].forceActiveFocus(Qt.TabFocusReason);
    }
    // The part the keys are at, in sight.
    function bringIntoSight(part: Item) {
        let up = part;
        while (up !== null && up !== rows)
            up = up.parent;
        if (up === null)
            return;
        const top = part.mapToItem(rows, 0, 0).y + 14;
        if (top - 14 < scroller.contentY)
            scroller.contentY = Math.max(0, top - 14);
        else if (top + part.height + 14 > scroller.contentY + scroller.height)
            scroller.contentY = Math.min(scroller.contentHeight - scroller.height, top + part.height + 14 - scroller.height);
    }

    x: parent.width - width
    y: parent.height + 8
    width: Math.ceil(widest) + 28
    height: Math.min(rows.implicitHeight + 28, room)
    padding: 0
    focus: true
    // (Not on a press on its button: that one's click closes it, as the page's toggles.)
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
    onAboutToShow: {
        const window = T.Overlay.overlay;
        room = window ? Math.max(100, window.height - parent.mapToItem(null, 0, parent.height).y - 8 - 8) : 100000;
        scroller.contentY = 0;
        // (Not where the pointer is over the button: a click, whatever the keys did before.)
        byKeys = ((parent as T.Control)?.visualFocus ?? false) && !((parent as T.Control)?.hovered ?? false);
    }
    onOpened: {
        if (panel.first !== null)
            panel.first.forceActiveFocus(panel.byKeys ? Qt.TabFocusReason : Qt.PopupFocusReason);
    }
    // (Qt gives the keys back to the button as it closes, and as a popup's: shown again a turn
    // later, where they are still there.)
    onClosed: {
        const button = parent as T.Control;
        if (panel.byKeys && button !== null)
            Qt.callLater(() => {
                if (button.activeFocus)
                    button.focusReason = Qt.TabFocusReason;
            });
    }

    background: Rectangle {
        radius: Theme.radius + 2
        color: Theme.panel
        border.width: 1
        border.color: Theme.lineStrong
    }

    Connections {
        function onActiveFocusItemChanged() {
            if (panel.opened)
                panel.bringIntoSight(panel.contentItem.Window.activeFocusItem);
        }

        target: panel.contentItem.Window.window
    }

    component Heading: Text {
        textFormat: Text.PlainText
        color: Theme.muted
        font.pixelSize: Theme.headingSize
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.9
    }
    component Divider: Rectangle {
        x: -14
        width: (parent ? parent.width : 0) + 28
        height: 1
        color: Theme.line
    }
    // As high as a button, and 4 px into the room above and below it, as the page's ".menu-row"
    // is. The keys go to its button.
    component Row: FocusScope {
        id: slot

        property alias text: pressed.text
        property alias symbol: pressed.symbol
        property alias key: pressed.key

        signal clicked

        width: parent ? parent.width : 0
        implicitWidth: pressed.implicitWidth
        implicitHeight: Theme.controlHeight - 8

        MenuRow {
            id: pressed

            y: -4
            width: slot.width
            focus: true
            onClicked: slot.clicked()
        }
    }

    // (Its parts are not given as its contentItem, which is made later than they are.)
    contentData: Flickable {
        id: scroller

        readonly property bool scrolls: contentHeight > height + 1

        anchors.fill: parent

        contentHeight: rows.implicitHeight + 28
        clip: true
        interactive: scrolls
        boundsBehavior: Flickable.StopAtBounds

        // (The mark of ScrollMark.qml, drawn over the content it is the mark of.)
        Rectangle {
            parent: scroller
            visible: scroller.scrolls
            x: scroller.width - width - 2
            y: scroller.visibleArea.yPosition * scroller.height
            width: 3
            height: Math.max(16, scroller.visibleArea.heightRatio * scroller.height)
            radius: 1.5
            color: Theme.lineStrong
        }

        Column {
            id: rows

            x: 14
            y: 14
            width: scroller.width - 28
            spacing: 12
            Keys.onUpPressed: panel.step(-1)
            Keys.onDownPressed: panel.step(1)
        }
    }
}
