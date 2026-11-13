pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import Qkvm

// All / Online / No signal / Attention: the wall's counts, which are also what it can be narrowed
// to. One control, as the page has it: the chosen one stands out, the others are quiet; No signal
// and Attention stand out of the way while nothing is in them, unless one is what is chosen; and
// what is chosen, pressed again, gives way to All (app.js, renderFilters).
Rectangle {
    id: filters

    required property WallModel machines
    // "all", "online", "no_signal" or "attention".
    property string chosen: "all"
    // Where there is no room for them, a state is its dot and its count without its word.
    property bool words: true
    // Counts of thousands are written short: "1.2k", "12k", "3M" (and said whole to whoever
    // cannot see them).
    property bool compact: false

    // A count as it is written. (In tenths of a thousand only where the wall has fewer than ten
    // thousand: on a wall of 12,000, "12k", a state of 9,500 is "9k", and no count is written in
    // more signs than the wall's own.)
    function written(count: int): string {
        if (!compact || count < 1000)
            return String(count);
        if (count < 10000 && machines.all < 10000)
            return String(Math.floor(count / 100) / 10) + "k";
        if (count < 1000000)
            return String(Math.floor(count / 1000)) + "k";
        return String(Math.floor(count / 1000000)) + "M";
    }

    // As many signs as the widest count up to this one is written in: as many as the wall's own,
    // but on a wall of a million or more, whose own is "1M" and a state's up to "999k".
    function longestWritten(count: int): int {
        return compact && count >= 1000000 ? 4 : written(count).length;
    }
    // The room each filter keeps for its count.
    readonly property real countRoom: Math.ceil(widestFigure * longestWritten(machines.all))

    signal picked(string what)

    // The widest figure of a count, as a count is written (figures, below).
    readonly property real widestFigure: {
        let widest = 0;
        for (let at = 0; at < figures.count; ++at) {
            const figure = figures.itemAt(at);
            if (figure !== null)
                widest = Math.max(widest, figure.implicitWidth);
        }
        return widest;
    }

    // How much wider the four are than where the wall's count has two figures: the bar takes it out
    // of its room for the rest (TopBar.qml), whose widths are measured on a wall of two.
    readonly property real beyondTwoFigures: Math.max(0, choices.length * (Math.ceil(widestFigure * longestWritten(machines.all)) - Math.ceil(widestFigure * 2)))

    // How wide each of the four is where it is shown, as each says (widthShown, below); and so how
    // wide it is with all four there: what the bar keeps for it, so that one that comes or goes
    // moves nothing beside it.
    property var widthsShown: [0, 0, 0, 0]
    readonly property real widthWithAll: Math.ceil(6 + row.spacing * (widthsShown.length - 1) + widthsShown.reduce((sum, width) => sum + width, 0))

    function shownAs(at: int, width: real) {
        if (widthsShown[at] === width)
            return;
        const now = widthsShown.slice();
        now[at] = width;
        widthsShown = now;
    }

    readonly property var choices: [
        {
            "key": "all",
            "label": Words.all,
            "count": machines.all,
            "dot": ""
        },
        {
            "key": "online",
            "label": Words.online,
            "count": machines.online,
            "dot": "online"
        },
        {
            "key": "no_signal",
            "label": Words.noSignal,
            "count": machines.noSignal,
            "dot": "no_signal"
        },
        {
            "key": "attention",
            "label": Words.attention,
            "count": machines.attention,
            "dot": "offline"
        }
    ]

    // Each figure as a count is written, to be measured; none is drawn.
    Repeater {
        id: figures

        model: 10

        delegate: Text {
            required property int index

            visible: false
            textFormat: Text.PlainText
            text: String(index)
            font.pixelSize: Theme.textSize
            font.weight: Font.DemiBold
        }
    }

    implicitWidth: row.implicitWidth + 6
    implicitHeight: Theme.controlHeight
    radius: Theme.radius
    color: Theme.inset
    border.width: 1
    border.color: Theme.lineStrong

    Row {
        id: row

        anchors.centerIn: parent
        spacing: 2

        Repeater {
            id: buttons

            model: filters.choices

            delegate: T.Button {
                id: choice

                required property var modelData
                required property int index
                readonly property bool isChosen: filters.chosen === modelData.key
                // As wide as it is where it is shown, also while it is not (what it holds is not
                // laid out then): its dot, its word, its count, the Row's 6 between them, and 20.
                // The count as wide as the widest figure, as many times as the wall's whole count
                // has figures: a count that changes, by one or by a figure more, moves nothing
                // beside it either, while the wall keeps its number of figures.
                readonly property real widthShown: {
                    const parts = [modelData.dot !== "" ? 8 : -1, filters.words || modelData.dot === "" ? Math.ceil(word.implicitWidth) : -1, Math.ceil(Math.max(number.implicitWidth, filters.widestFigure * filters.longestWritten(filters.machines.all)))].filter(part => part >= 0);
                    return parts.reduce((sum, part) => sum + part, 0) + 6 * (parts.length - 1) + 20;
                }
                onWidthShownChanged: filters.shownAs(index, widthShown)
                Component.onCompleted: filters.shownAs(index, widthShown)

                objectName: "filter-" + modelData.key
                visible: modelData.key === "all" || modelData.key === "online" || modelData.count > 0 || isChosen
                implicitWidth: inside.implicitWidth + 20
                implicitHeight: 24
                Accessible.name: modelData.label + " " + modelData.count
                Accessible.checkable: true
                Accessible.checked: isChosen
                onClicked: {
                    // A chosen one with nothing in it goes as it is no longer chosen: the keyboard
                    // goes to All, and is not left on what went.
                    const goes = isChosen && modelData.key !== "all" && modelData.key !== "online" && modelData.count === 0;
                    const hadTheKeyboard = choice.activeFocus;
                    const how = choice.focusReason;
                    filters.picked(isChosen ? "all" : modelData.key);
                    if (goes && hadTheKeyboard)
                        buttons.itemAt(0).forceActiveFocus(how);
                }

                contentItem: Item {
                    Row {
                        id: inside

                        anchors.centerIn: parent
                        spacing: 6

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: choice.modelData.dot !== ""
                            width: 8
                            height: 8
                            radius: 4
                            color: Theme.stateColor(choice.modelData.dot)
                        }
                        Text {
                            id: word

                            textFormat: Text.PlainText
                            anchors.verticalCenter: parent.verticalCenter
                            visible: filters.words || choice.modelData.dot === ""
                            text: choice.modelData.label
                            color: choice.isChosen || choice.hovered ? Theme.text : Theme.muted
                            font.pixelSize: Theme.textSize
                            font.weight: Font.Medium
                        }
                        Text {
                            id: number

                            objectName: "filterCount"
                            textFormat: Text.PlainText
                            anchors.verticalCenter: parent.verticalCenter
                            text: filters.written(choice.modelData.count)
                            color: Theme.text
                            font.pixelSize: Theme.textSize
                            font.weight: Font.DemiBold
                        }
                    }
                }
                background: Rectangle {
                    radius: 3
                    color: choice.isChosen ? Theme.panel : "transparent"
                    border.width: choice.isChosen ? 1 : 0
                    border.color: Theme.line

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: 6
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.accentText
                        visible: choice.visualFocus
                    }
                }
            }
        }
    }
}
