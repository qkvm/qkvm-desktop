pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import Qkvm

// A row of the console's controls that moves the least needed of them, intact, into More where the
// row has no room for them all (by `priority`; never one that is `pinned`), as the page's console
// does (app.js, setupConsoleOverflow). The one that is `last` stands at the row's right end, after
// More. `packed`: the rest stand at the right too, beside More, as the page's console tools do
// (app.css, ".focus-tools"); otherwise from the left.
// It is fitted as it is laid out: once it has its width, and at once when that changes, before the
// window draws it, and at once when a control's words change its width. (A turn later, it was drawn
// unfitted first; and fitted before it had a width at all, every control that may go went into More,
// and More stood over those that stayed.) What it offers is fitted a turn after it changes, all of a
// change together.
// The keys go with a control that is moved: into More where More is closed, and back to it when it
// comes back; where More goes with them, to the next control in Tab's order. Never to More unseen.
// Each control has a place of its own in More as in the row, in the row's order: Tab and a screen
// reader go through More as it is seen, whatever order its controls came in.
Item {
    id: shelf

    property list<ConsoleAction> items
    property bool packed: false
    property string menuName
    property string description: qsTr("More console controls")
    property bool localKeys: true
    readonly property bool opened: popup.visible
    property bool fitting: false
    property bool placed: false
    property var overflow: []
    // The control that gave More the keys as it went into it, while More keeps them.
    property ConsoleAction keysFrom: null
    // How wide the row is with all it offers in it, and no More.
    readonly property real full: {
        const shown = items.filter(item => item.offered);
        return shown.reduce((sum, item) => sum + item.implicitWidth, 0) + Math.max(0, shown.length - 1) * 6;
    }
    signal interactionStarted
    signal interactionEnded

    implicitHeight: Theme.controlHeight

    function schedule() {
        fitLater.start();
    }
    function closeMenu() {
        popup.close();
    }
    // Where a control stands: a place of its own, in the row's order, in the row or in More. (Each has
    // one from the start; were one missing, it would stand in the row or More itself, and be moved
    // again at every fit: said, not to go unseen.)
    function placeOf(item, inMenu) {
        const at = items.indexOf(item);
        const place = inMenu ? menuPlaces.itemAt(at) : item.last ? lastPlace : places.itemAt(at);
        if (place)
            return place;
        console.warn("ConsoleOverflow", shelf.menuName, "has no place for", item.objectName);
        return inMenu ? menuItems : shelf;
    }
    function fit() {
        fitLater.stop();
        // (Not before it is laid out: with no width, every control that may go would go into More.)
        if (fitting || !placed || width <= 0)
            return;
        fitting = true;
        const shown = items.filter(item => item.offered);
        const gaps = 6;
        let used = shown.reduce((sum, item) => sum + item.implicitWidth, 0) + Math.max(0, shown.length - 1) * gaps;
        const moved = [];
        if (used > width) {
            used += more.implicitWidth + gaps;
            const candidates = shown.filter(item => !item.pinned).sort((a, b) => a.priority - b.priority || items.indexOf(b) - items.indexOf(a));
            for (const item of candidates) {
                if (used <= width)
                    break;
                moved.push(item);
                used -= item.implicitWidth + gaps;
            }
        }
        overflow = items.filter(item => moved.includes(item));
        // (Packed: from where the row, More and the last one end, back.)
        const last = shown.find(item => item.last);
        const inRow = shown.filter(item => !moved.includes(item) && !item.last);
        const rowWidth = inRow.reduce((sum, item) => sum + item.implicitWidth, 0) + Math.max(0, inRow.length - 1) * gaps;
        const end = width - (last ? last.implicitWidth + gaps : 0) - (moved.length ? more.implicitWidth + gaps : 0);
        let left = packed ? Math.max(0, end - rowWidth) : 0;
        let top = 0;
        for (const item of items) {
            const inMenu = moved.includes(item);
            // Only a control that goes into More, or comes back from it, is moved: one that is moved loses
            // the keys for a moment, and a key or a button held down on it is let go of with them.
            const place = placeOf(item, inMenu);
            if (item.parent !== place) {
                // (The keys go with it, as they came: outlined where they were.)
                const keys = item.activeFocus;
                const reason = item.visualFocus ? Qt.TabFocusReason : Qt.OtherFocusReason;
                item.parent = place;
                if (keys && inMenu && !popup.visible) {
                    more.forceActiveFocus(reason);
                    keysFrom = item;
                } else if (keys) {
                    item.forceActiveFocus(reason);
                }
            }
            item.width = inMenu ? menuItems.width : item.implicitWidth;
            item.height = Theme.controlHeight;
            item.x = inMenu ? 0 : item.last ? width - item.width : left;
            item.y = inMenu ? top : 0;
            if (item.offered) {
                if (inMenu)
                    top += item.height + 4;
                else if (!item.last)
                    left += item.width + gaps;
            }
        }
        more.x = Math.max(0, width - more.width - (last ? last.width + gaps : 0));
        menuItems.height = Math.max(0, top - 4);
        // More keeps keys a control gave it only while that control is in it, and none once it is not
        // seen: back to the control, or on to the next in Tab's order.
        if (more.activeFocus) {
            const reason = more.visualFocus ? Qt.TabFocusReason : Qt.OtherFocusReason;
            const back = keysFrom;
            if (back && !moved.includes(back) && back.offered && back.enabled)
                back.forceActiveFocus(reason);
            if (more.activeFocus && !more.visible) {
                const next = more.nextItemInFocusChain(true);
                if (next && next !== more)
                    next.forceActiveFocus(reason);
            }
        }
        if (!overflow.length)
            popup.close();
        fitting = false;
    }

    onWidthChanged: fit()
    onItemsChanged: schedule()
    Component.onCompleted: {
        // Each to its place before anything has the keys: Tab goes as the eye does then, the row's
        // controls in their order, More, and the last one; and one back from More comes to its own
        // place, the others staying where they are.
        for (const item of items) {
            item.shelf = shelf;
            item.parent = placeOf(item, false);
        }
        placed = true;
        fit();
    }

    // (A fit asked a turn before the row goes, as a machine shown large is closed, goes with it: the
    // row's own timer asks it, not a call that outlives the row.)
    Timer {
        id: fitLater

        interval: 0
        onTriggered: shelf.fit()
    }
    // The places of the row's controls, in its order, and More after them, and the last one's after
    // More. (Each at the row's top left corner: a control in it stands where the row puts it.)
    Repeater {
        id: places

        model: shelf.items.length

        Item {}
    }
    Button {
        id: more
        objectName: shelf.menuName + "Button"
        visible: shelf.overflow.length > 0
        text: qsTr("More")
        hint: shelf.description
        drops: true
        focusPolicy: shelf.localKeys ? Qt.StrongFocus : Qt.NoFocus
        checked: popup.visible
        // (Holding nothing, it opens nothing, however the keys came to it.)
        onClicked: {
            if (popup.visible)
                popup.close();
            else if (shelf.overflow.length)
                popup.open();
        }
        // (A key held on a control as it goes into More repeats here: its repeat presses nothing, and
        // its release opens nothing.)
        Keys.onPressed: event => {
            if (event.isAutoRepeat && (event.key === Qt.Key_Space || event.key === Qt.Key_Select))
                event.accepted = true;
        }
        onActiveFocusChanged: {
            if (!activeFocus)
                shelf.keysFrom = null;
        }
        T.Popup {
            id: popup
            objectName: shelf.menuName
            x: more.width - width
            y: more.height + 4
            width: Math.min(320, Math.max(200, ...shelf.overflow.map(item => item.implicitWidth + 16)))
            height: Math.min(menuItems.height + 16, Math.max(80, (T.Overlay.overlay ? T.Overlay.overlay.height : 400) - more.mapToItem(null, 0, more.height).y - 12))
            padding: 8
            focus: true
            // (A press on More itself is its click's: a second one closes it, as the bar's drops.)
            closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
            onAboutToShow: shelf.interactionStarted()
            onOpened: {
                const first = shelf.overflow.find(item => item.enabled);
                if (first)
                    first.forceActiveFocus();
            }
            onClosed: shelf.interactionEnded()
            background: Rectangle {
                color: Theme.panel
                radius: Theme.radius
                border.color: Theme.lineStrong
            }
            contentItem: Flickable {
                clip: true
                contentHeight: menuItems.height
                boundsBehavior: Flickable.StopAtBounds
                Item {
                    id: menuItems
                    width: popup.availableWidth

                    // The places of the controls in More, in the row's order: Tab and a screen reader
                    // go through them as they are shown, whatever order they came in.
                    Repeater {
                        id: menuPlaces

                        model: shelf.items.length

                        Item {}
                    }
                }
            }
        }
    }
    Item {
        id: lastPlace
    }
}
