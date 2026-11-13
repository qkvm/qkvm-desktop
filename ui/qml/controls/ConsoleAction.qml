import QtQuick
import Qkvm

// One control, moved intact between its row and More. State and handlers stay on it.
Button {
    property bool offered: true
    property int priority: 50
    property bool pinned: false
    property bool last: false
    property var shelf: null
    // It does nothing just now, and says so by its look, as a button that cannot be pressed does,
    // while it keeps the keys where it has them. (Whoever uses it does nothing at its click then.)
    property bool resting: false

    visible: offered
    opacity: enabled && !resting ? 1 : 0.5
    onOfferedChanged: if (shelf)
        shelf.schedule()
    onImplicitWidthChanged: if (shelf)
        shelf.schedule()
    onClicked: if (shelf)
        shelf.closeMenu()
}
