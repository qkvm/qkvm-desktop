pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

// How the wall is shown, dropping from the bar's View as the page's View panel does (index.html,
// "view-panel"): three rows, each its small heading over a choice (Order, Columns, A new picture
// every); "Arrange the tiles..." for whoever manages the wall; a line; "Display mode" (D); and the
// program's own "New window" (Ctrl+N). A choice is made at once and the panel stays, as the page's
// select does ("order", "columns", "pace"); a row that leads somewhere closes it first ("arrange",
// "display", "window").
Panel {
    id: menu

    required property WallView wall
    property bool manages: false

    first: order

    Column {
        width: parent.width
        spacing: 5

        Panel.Heading {
            text: Words.order
        }
        Choice {
            id: order

            objectName: "orderChoice"
            width: parent.width
            arrowsStep: true
            symbol: "list"
            hint: Words.order
            choices: [
                {
                    "value": "",
                    "label": Words.asArranged
                },
                {
                    "value": "name",
                    "label": Words.byName
                },
                {
                    "value": "group",
                    "label": Words.byGroup
                },
                {
                    "value": "state",
                    "label": Words.problemsFirst
                }
            ]
            value: menu.wall.shown.order
            onPicked: value => menu.picked("order", value)
        }
    }
    Column {
        width: parent.width
        spacing: 5

        Panel.Heading {
            text: Words.columns
        }
        Choice {
            objectName: "columnsChoice"
            width: parent.width
            arrowsStep: true
            symbol: "columns"
            hint: Words.columns
            choices: [0, 1, 2, 3, 4, 5, 6].map(count => ({
                        "value": count,
                        "label": count === 0 ? Words.fit : String(count)
                    }))
            value: menu.wall.columns
            onPicked: value => menu.picked("columns", value)
        }
    }
    Column {
        width: parent.width
        spacing: 5

        Panel.Heading {
            text: Words.aNewPictureEvery
        }
        Choice {
            objectName: "paceChoice"
            width: parent.width
            arrowsStep: true
            symbol: "clock"
            hint: Words.aNewPictureEvery
            choices: menu.wall.paces.map(every => ({
                        "value": every,
                        "label": every === 0 ? Words.paused : Words.seconds.arg(every / 1000)
                    }))
            value: menu.wall.pace
            onPicked: value => menu.picked("pace", value)
        }
    }
    Panel.Row {
        visible: menu.manages
        symbol: "grid"
        text: Words.arrangeTheTiles
        onClicked: menu.pick("arrange", true)
    }
    Panel.Divider {}
    Panel.Row {
        symbol: "tv"
        text: Words.displayMode
        key: "D"
        onClicked: menu.pick("display", true)
    }
    Panel.Row {
        symbol: "external"
        text: Words.newWindow
        key: "Ctrl+N"
        onClicked: menu.pick("window", true)
    }
}
