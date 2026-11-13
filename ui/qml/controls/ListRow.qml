pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// One row of a list (Listed): what it is, in the list's columns (`cells`: one {text, under, tone} for
// each, `under` a list of what is said under what it is, in its order; `tone` "bad" writes that in
// the colour of a problem, and a list of tones has one for each of `under`), and at its right end
// what one does to it (what the row is given), in the list's column for that. Where the window is
// narrow, its columns stand one under the other, and what one does stands under them.
//
// It is as high as every other row of its list, whatever it says: each cell has the lines its column
// has (Listed's `lines`), filled from the top, and what is longer is cut at their end. All that it
// says is whole in `whole`, which is its tooltip and its name for a reader of the screen.
Item {
    id: row

    // The list it is a row of: how wide its columns are, and how many lines each has. And which of
    // its rows it is, counted from 0, which says where it stands among all of them.
    required property Listed of
    required property int index
    property var cells: []
    default property alias does: doing.data

    // Which of the shapes the settings are made of this is (Section.qml).
    readonly property string shape: "row"
    // All that it says, whole, a line for each of what stands in its cells.
    readonly property string whole: cells.reduce((all, cell) => all.concat([cell.text ?? ""], cell.under ?? []), []).filter(said => said !== "").join("\n")

    // Whether some of what it says is cut where it stands.
    function isCut(): bool {
        const cut = item => {
            for (let at = 0; at < item.children.length; ++at) {
                const child = item.children[at];
                if (child.truncated === true || cut(child))
                    return true;
            }
            return false;
        };
        return cut(table);
    }
    // The first of what one does to it that takes a press; null: none does.
    function firstThatTakesAPress(): Item {
        for (let at = 0; at < doing.children.length; ++at) {
            const one = doing.children[at];
            if (one.visible && one.enabled && one.activeFocusOnTab !== false)
                return one;
        }
        return null;
    }

    y: index * of.rowHeight
    width: of.width - 2
    height: of.rowHeight
    Component.onCompleted: of.made += 1
    Accessible.role: Accessible.Row
    Accessible.name: whole

    HoverHandler {
        objectName: "rowPointer"
        onHoveredChanged: row.of.pointerOn(row, hovered)
    }
    // Between it and what stands above it in its list (every row but the first).
    Rectangle {
        width: parent.width
        height: 1
        visible: row.index > 0
        color: Theme.line
    }
    Grid {
        id: table

        x: 18
        y: 12
        columns: row.of.narrow ? 1 : Math.max(1, row.cells.length)
        columnSpacing: 16
        rowSpacing: 8

        Repeater {
            model: row.cells

            delegate: Column {
                id: cell

                required property var modelData
                required property int index
                readonly property var lines: row.of.linesOf(index)

                width: row.of.narrow ? row.width - 36 : row.of.cellWidth
                height: row.of.cellHeight(index)
                spacing: 2

                Text {
                    textFormat: Text.PlainText
                    objectName: "rowText"
                    width: parent.width
                    height: cell.lines.text * row.of.textLine
                    text: cell.modelData.text ?? ""
                    color: Theme.text
                    font.pixelSize: Theme.textSize
                    wrapMode: Text.Wrap
                    maximumLineCount: cell.lines.text
                    elide: Text.ElideRight
                }
                Repeater {
                    model: cell.lines.under

                    delegate: Text {
                        id: under

                        required property int modelData
                        required property int index

                        textFormat: Text.PlainText
                        objectName: "rowUnder"
                        width: cell.width
                        height: modelData * row.of.smallLine
                        text: (cell.modelData.under ?? [])[under.index] ?? ""
                        color: (typeof cell.modelData.tone === "object" && cell.modelData.tone !== null ? cell.modelData.tone[under.index] : cell.modelData.tone) === "bad" ? Theme.bad : Theme.muted
                        font.pixelSize: Theme.smallSize
                        wrapMode: Text.Wrap
                        maximumLineCount: modelData
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
    Row {
        id: doing

        objectName: "rowDoes"
        x: row.of.narrow ? 18 : row.width - 18 - width
        y: row.of.narrow ? 12 + row.of.cellsHeight + 8 : Math.round((row.height - height) / 2)
        spacing: 8
    }
}
