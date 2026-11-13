pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import Qkvm

// A list in a section of the settings, the page's "listed": in a frame as lines are, a head that says
// what each column is (`columns`), then one row (ListRow) for each of the model's, with what one does
// to a row at that row's right end, in a column of its own as wide as `doesWidth`; or one line that
// says there is nothing in it yet (`nothing`). A small heading above it where it has one (`title`).
// Where the window is narrow, a row's columns stand one under the other, and the head is not there.
//
// Every row is of one height, from the start: each column's cell has so many lines, whatever is in
// them (`lines`: for each column, how many lines what it is takes, `text`, and how many each of what
// is said under that takes, `under`, in their order; a column that says nothing: one line, nothing
// under it). What is longer than its lines is cut at their end, and is whole in the row's tooltip
// and in its name for a reader of the screen. So nothing that comes late and no name however long
// changes the height of a row, and no row moves for what another says.
//
// And so the list knows how high it is, all of it, without making its rows: it makes those that
// are in sight in what scrolls it (the section's content), and a thousand pixels' worth above and
// below, which is where the keyboard's Tab goes next; and makes the others as they come into sight.
// Each row it makes stands at its own place among all of them, by its number (its `index`), one
// row's height under the one before it. A wall holds two thousand KVMs. The row the keyboard rests
// in stays made however far it is scrolled out of sight (the keyboard would go to nothing with it),
// and so it does while the keyboard is where this list is not looked at, and comes back from: out
// of the window (another program is in front), or in a box over the section (the one its Edit
// opened). It is let go as any other once the keyboard has come to rest on another item of the
// section the list stands in.
Column {
    id: listed

    property string title
    property var columns: []
    property var lines: []
    property real doesWidth: 0
    property string nothing
    property alias model: rows.model
    property alias delegate: rows.delegate
    readonly property int count: rows.items.count
    // What scrolls the list, found where it stands; none: all of it is in sight, and its rows made.
    // And whether that was looked for yet: until it was, no row is taken to be in sight (so none is
    // made for a moment in which it would seem that all of them are).
    property Flickable within: null
    property bool looked: false
    // How many rows were made since the list was: for whoever wants to know how many it makes.
    property int made: 0
    // (What is in sight is to be made a turn of the loop from now; the list itself changed since what
    // was made last; and the numbers of the first and the last row that were made.)
    property bool making: false
    property bool madeAnew: true
    property point madeFromTo: Qt.point(0, -1)
    // (The number of the row that is made beyond those, because the keyboard was in it; -1: none.)
    property int keptForTheKeyboard: -1
    // The row the keyboard rests in, or rested in last before it went where this list is not looked
    // at (keyboardCameToRest); null: none, since it came to rest on another item of the section.
    property ListRow keyboardsRow: null
    // The section the list stands in, found where it stands; none: the list itself is all of it.
    property Item standsIn: null

    // Which of the shapes the settings are made of this is (Section.qml).
    readonly property string shape: "listed"
    readonly property bool narrow: Window.width > 0 && Window.width < Theme.narrowWindow
    // How wide each column is, beside the others and the column of what one does: and where each
    // begins, in a row.
    readonly property real cellWidth: Math.max(40, (width - 2 - 36 - (doesWidth > 0 ? doesWidth + 24 : 0) - 16 * Math.max(0, columns.length - 1)) / Math.max(1, columns.length))
    // How high a line of what a row says is, of each of its two sizes; and so how high a cell of each
    // column is, all of a row's cells, and a row.
    readonly property real textLine: Math.ceil(textSize.lineSpacing)
    readonly property real smallLine: Math.ceil(smallSize.lineSpacing)
    readonly property real cellsHeight: {
        const heights = [];
        for (let column = 0; column < Math.max(1, columns.length); ++column)
            heights.push(cellHeight(column));
        return narrow ? heights.reduce((all, one) => all + one, 0) + 8 * (heights.length - 1) : Math.max(...heights);
    }
    readonly property real rowHeight: narrow ? 24 + cellsHeight + (doesWidth > 0 ? 8 + Theme.controlHeight : 0) : 24 + Math.max(cellsHeight, doesWidth > 0 ? Theme.controlHeight : 0)

    function cellAt(index: int): real {
        return 18 + index * (cellWidth + 16);
    }
    // How many lines a column's cell has: {text, under: [each of what is under it]}.
    function linesOf(column: int): var {
        const said = lines[column] ?? {};
        return {
            "text": said.text ?? 1,
            "under": said.under ?? []
        };
    }
    function cellHeight(column: int): real {
        const of = linesOf(column);
        return of.text * textLine + of.under.reduce((all, under) => all + 2 + under * smallLine, 0);
    }
    // The row of the model's row of this number; null where there is none, or it is not made (it is
    // far out of sight).
    function rowAt(index: int): ListRow {
        for (let at = 0; at < rowsMade.count; ++at) {
            const one = rowsMade.itemAt(at) as ListRow;
            if (one !== null && one.index === index)
                return one;
        }
        return null;
    }
    // The first of the rows that are made for which `test` says yes; null: none.
    function rowWhere(test): ListRow {
        for (let at = 0; at < rowsMade.count; ++at) {
            const one = rowsMade.itemAt(at) as ListRow;
            if (one !== null && test(one))
                return one;
        }
        return null;
    }
    // Where the keyboard has come to rest: on an item of the section the list stands in, in one of
    // this list's rows or elsewhere there, which is where it rests from now on. Nowhere (the window
    // is not the one in front) and out of the section (in a box over it) are no rest: the row it
    // rested in is the one it comes back to.
    function keyboardCameToRest() {
        const within = listed.standsIn ?? listed;
        let row = null;
        for (let up = listed.Window.activeFocusItem; up !== null; up = up.parent) {
            if (row === null)
                row = up as ListRow;
            if (up === within) {
                keyboardsRow = row !== null && row.of === listed ? row : null;
                return;
            }
        }
    }
    // The rows that are in sight, and a thousand pixels' worth beyond, are made, and no others but the
    // one the keyboard rests in. Asked a turn of the loop after what is in sight, the list, or where
    // the keyboard is changed: once for all that changed in that turn, and never while the list is
    // being changed. (Rows that stay in sight stay as they are.)
    function makeWhatIsInSight(anew: bool) {
        if (anew)
            madeAnew = true;
        if (making)
            return;
        making = true;
        Qt.callLater(() => {
            making = false;
            const all = rows.items.count;
            const from = Math.min(allRows.first, all);
            const to = Math.min(allRows.last, all - 1);
            const keep = keyboardsRow !== null ? keyboardsRow.index : -1;
            // Those made before and out of sight now go, but the one the keyboard is in; those in
            // sight now and not made before are made. (Where the list itself changed, what was made
            // is not known: all of it is looked at once.)
            const wasAnew = madeAnew;
            const was = madeAnew ? Qt.point(0, all - 1) : madeFromTo;
            madeAnew = false;
            const drop = (a, b) => {
                if (b >= a && a < all)
                    rows.items.removeGroups(a, Math.min(b, all - 1) - a + 1, "inSight");
            };
            const dropAllBut = (a, b) => {
                if (keep >= a && keep <= b) {
                    drop(a, keep - 1);
                    drop(keep + 1, b);
                } else {
                    drop(a, b);
                }
            };
            dropAllBut(was.x, Math.min(was.y, from - 1));
            dropAllBut(Math.max(was.x, to + 1), was.y);
            // The one that was kept for the keyboard before, out of sight, and the keyboard came to
            // rest elsewhere in the section since.
            const kept = keptForTheKeyboard;
            if (!wasAnew && kept >= 0 && kept !== keep && (kept < from || kept > to))
                drop(kept, kept);
            if (to >= from)
                rows.items.addGroups(from, to - from + 1, "inSight");
            madeFromTo = Qt.point(from, to);
            keptForTheKeyboard = keep >= 0 && (keep < from || keep > to) ? keep : -1;
        });
    }
    // The pointer rests on a row, or no longer: where some of what it says is cut, what it says
    // whole is shown by it a moment later.
    function pointerOn(row: ListRow, on: bool) {
        if (on) {
            tip.close();
            resting.row = row;
            resting.restart();
        } else if (resting.row === row) {
            resting.stop();
            resting.row = null;
            tip.close();
        }
    }

    width: parent ? parent.width : 0
    spacing: 10
    Component.onCompleted: {
        for (let up = parent; up !== null && standsIn === null; up = up.parent) {
            if (within === null)
                within = up as Flickable;
            if ((up as Section) !== null)
                standsIn = up;
        }
        looked = true;
        keyboardCameToRest();
    }

    FontMetrics {
        id: textSize

        font.pixelSize: Theme.textSize
    }
    FontMetrics {
        id: smallSize

        font.pixelSize: Theme.smallSize
    }
    Timer {
        id: resting

        property ListRow row: null

        interval: 600
        onTriggered: {
            if (row !== null && row.isCut()) {
                tip.parent = row;
                tip.text = row.whole;
                tip.open();
            }
        }
    }
    Tip {
        id: tip

        objectName: "listedTip"
    }
    // The model's rows, as rows of the list: all of them are counted, and those that are in the
    // group "inSight" are made.
    DelegateModel {
        id: rows

        filterOnGroup: "inSight"
        groups: [
            DelegateModelGroup {
                name: "inSight"
            }
        ]
    }
    // A row added, or the list taken anew: what is in sight is made of it.
    Connections {
        function onChanged() {
            listed.makeWhatIsInSight(true);
        }

        target: rows.items
    }
    // The keyboard moved: where it came to rest is noted; and a row that was kept for it out of sight,
    // where it rests elsewhere in the section now, goes as the others did.
    Connections {
        function onActiveFocusItemChanged() {
            listed.keyboardCameToRest();
            if (listed.keptForTheKeyboard >= 0)
                listed.makeWhatIsInSight(false);
        }

        target: listed.Window
    }

    // The page's small heading: capitals, spaced, quiet.
    Text {
        textFormat: Text.PlainText
        objectName: "listedTitle"
        width: parent.width
        visible: listed.title !== ""
        text: listed.title
        color: Theme.muted
        font.pixelSize: Theme.headingSize
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.9
        elide: Text.ElideRight
    }
    Rectangle {
        width: parent.width
        height: inside.height + 2
        radius: Theme.radius + 2
        color: Theme.lines
        border.width: 1
        border.color: Theme.line

        Column {
            id: inside

            objectName: "listedFrame"
            x: 1
            y: 1
            width: parent.width - 2

            // What each column is.
            Rectangle {
                objectName: "listedHead"
                width: parent.width
                height: 34
                visible: listed.count > 0 && !listed.narrow && listed.columns.length > 0
                color: Theme.inset
                topLeftRadius: Theme.radius + 1
                topRightRadius: Theme.radius + 1

                Repeater {
                    model: listed.columns

                    delegate: Text {
                        required property string modelData
                        required property int index

                        textFormat: Text.PlainText
                        objectName: "listedColumn"
                        x: listed.cellAt(index)
                        width: listed.cellWidth
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: Theme.muted
                        font.pixelSize: Theme.headingSize
                        font.weight: Font.DemiBold
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.7
                        elide: Text.ElideRight
                    }
                }
            }
            // All of its rows, as high as they are together; and in it, where they are in sight (and
            // a little beyond), the rows themselves, each where it stands among all of them.
            Item {
                id: allRows

                // The part of them that is in sight, from where (x) to where (y), in their own place.
                // (Looked at anew as what scrolls them scrolls or changes size, and as what stands
                // above them in it moves: each of those is read here. One value for both ends: never
                // the one end anew and the other as it was, which would be all the rows for a moment,
                // and make every one of them.)
                readonly property point seen: {
                    const flickable = listed.within;
                    if (!listed.looked)
                        return Qt.point(0, 0);
                    if (flickable === null)
                        return Qt.point(0, height);
                    let at = 0;
                    for (let up = allRows as Item; up !== null && up !== flickable.contentItem; up = up.parent)
                        at += up.y;
                    return Qt.point(flickable.contentY - at, flickable.contentY - at + flickable.height);
                }

                // The numbers of the first and the last row to be made: those in sight, and a thousand
                // pixels' worth beyond each end (where Tab goes next, and what the wheel brings first).
                readonly property int first: listed.rowHeight > 0 ? Math.max(0, Math.floor((seen.x - 1000) / listed.rowHeight)) : 0
                readonly property int last: listed.rowHeight > 0 ? Math.min(listed.count - 1, Math.floor((seen.y + 1000) / listed.rowHeight)) : -1

                objectName: "listedRows"
                width: parent.width
                height: listed.count * listed.rowHeight
                onFirstChanged: listed.makeWhatIsInSight(false)
                onLastChanged: listed.makeWhatIsInSight(false)

                // (Each of them places itself, by its number: ListRow.qml.)
                Repeater {
                    id: rowsMade

                    model: rows
                }
            }
            // Nothing in it yet.
            Text {
                textFormat: Text.PlainText
                objectName: "listedNothing"
                width: parent.width
                visible: listed.count === 0
                topPadding: 22
                bottomPadding: 22
                leftPadding: 18
                rightPadding: 18
                text: listed.nothing
                color: Theme.muted
                font.pixelSize: Theme.textSize
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
        }
    }
}
