pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import Qkvm

// A row of the console's controls that moves the least needed of them, intact, into More where the
// row has no room for them all (by `priority`; never one that is `pinned`), as the page's console
// does (app.js, setupConsoleOverflow). The one that is `last` stands at the row's right end, after
// More. `packed`: the rest stand at the right too, beside More, as the page's console tools do
// (app.css, ".focus-tools"); otherwise from the left.
Item {
    id: shelf

    property list<ConsoleAction> items
    property bool packed: false
    property string menuName
    property string description: qsTr("More console controls")
    property bool localKeys: true
    readonly property bool opened: popup.visible
    property bool fitting: false
    property bool scheduled: false
    property var overflow: []
    // How wide the row is with all it offers in it, and no More.
    readonly property real full: {
        const shown = items.filter(item => item.offered);
        return shown.reduce((sum, item) => sum + item.implicitWidth, 0) + Math.max(0, shown.length - 1) * 6;
    }
    signal interactionStarted
    signal interactionEnded

    implicitHeight: Theme.controlHeight

    function schedule() {
        if (scheduled)
            return;
        scheduled = true;
        Qt.callLater(fit);
    }
    function closeMenu() {
        popup.close();
    }
    function fit() {
        scheduled = false;
        if (fitting)
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
            const hadFocus = item.activeFocus;
            item.parent = inMenu ? menuItems : shelf;
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
            if (hadFocus && inMenu && !popup.visible)
                more.forceActiveFocus();
        }
        more.x = Math.max(0, width - more.width - (last ? last.width + gaps : 0));
        menuItems.height = Math.max(0, top - 4);
        // Tab goes as the eye does: the row's controls in their order, then More, then the last one. (A
        // control back from More was put after all the others, and More, made first, stood before them.)
        // Put in that order where they are not, the keys staying where they were.
        const order = items.filter(item => !moved.includes(item) && !item.last);
        order.push(more);
        for (const item of items) {
            if (item.last && !moved.includes(item))
                order.push(item);
        }
        let at = 0;
        let inOrder = true;
        for (let child = 0; child < shelf.children.length; ++child) {
            if (order.includes(shelf.children[child]) && shelf.children[child] !== order[at++])
                inOrder = false;
        }
        if (!inOrder) {
            const focused = order.find(item => item.activeFocus);
            // (And their outline, where it showed them: they came from the keyboard.)
            const reason = focused && focused.visualFocus ? Qt.TabFocusReason : Qt.OtherFocusReason;
            for (const item of order) {
                item.parent = aside;
                item.parent = shelf;
            }
            // (Qt keeps the keys on a control that is put elsewhere in the same row, and says they came
            // there for no reason of the keyboard's: the reason is said again.)
            if (focused) {
                focused.forceActiveFocus(reason);
                focused.focusReason = reason;
            }
        }
        if (!overflow.length)
            popup.close();
        fitting = false;
    }

    onWidthChanged: schedule()
    onItemsChanged: schedule()
    Component.onCompleted: {
        for (const item of items)
            item.shelf = shelf;
        schedule();
    }

    // (Where a control stands for a moment, to be put back after the others: seen, as the row is,
    // so that nothing of it changes on the way.)
    Item {
        id: aside
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
        onClicked: popup.visible ? popup.close() : popup.open()
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
                }
            }
        }
    }
}
