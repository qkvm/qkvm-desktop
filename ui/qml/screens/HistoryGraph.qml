pragma ComponentBehavior: Bound

import QtQuick
import Qkvm

Column {
    id: graph
    required property var values
    spacing: 6
    readonly property var colors: Theme.dark ? [Theme.accentText, "#e6a261", "#6cc29b", "#ba8be0", "#e9879e", "#78becf", "#d5c65d", "#b0b0b0"] : [Theme.accentText, "#98501c", "#23764d", "#7d3dab", "#a33650", "#287286", "#75681d", "#555555"]
    Text {
        width: parent.width
        text: graph.values.title + " · " + graph.values.unit
        textFormat: Text.PlainText
        color: Theme.text
        font.pixelSize: Theme.textSize
        font.bold: true
    }
    Canvas {
        id: plot
        objectName: "historyPlot"
        width: parent.width
        height: 120
        Accessible.role: Accessible.Graphic
        Accessible.name: graph.values.title + qsTr(" history. Latest readings are listed below.")
        onWidthChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);
            const data = graph.values;
            const span = Math.max(1, data.until - data.since);
            const range = Math.max(1, data.high - data.low);
            const floor = data.high - data.low < 1 ? data.low - 0.5 : data.low;
            ctx.strokeStyle = Theme.lineStrong;
            ctx.lineWidth = 1;
            ctx.beginPath();
            for (let row = 0; row <= 2; ++row) {
                const y = 8 + row * (height - 16) / 2;
                ctx.moveTo(0, y);
                ctx.lineTo(width, y);
            }
            ctx.stroke();
            for (let line = 0; line < data.lines.length; ++line) {
                const points = data.lines[line].points;
                ctx.strokeStyle = graph.colors[line % graph.colors.length];
                ctx.lineWidth = 1.5;
                ctx.beginPath();
                for (let i = 0; i < points.length; ++i) {
                    const p = points[i];
                    const x = (p[0] - data.since) / span * width;
                    const y = height - 8 - (p[1] - floor) / range * (height - 16);
                    // A gap of more than two sampling periods is not continuous data.
                    if (i === 0 || p[0] - points[i - 1][0] > 1800)
                        ctx.moveTo(x, y);
                    else
                        ctx.lineTo(x, y);
                    ctx.fillStyle = ctx.strokeStyle;
                    ctx.fillRect(x - 1.5, y - 1.5, 3, 3);
                }
                ctx.stroke();
            }
        }
        Connections {
            target: graph
            function onValuesChanged() {
                plot.requestPaint();
            }
            function onColorsChanged() {
                plot.requestPaint();
            }
        }
    }
    Text {
        width: parent.width
        text: qsTr("Range: %1–%2 %3 · averages over the last day").arg(Number(graph.values.low).toFixed(1)).arg(Number(graph.values.high).toFixed(1)).arg(graph.values.unit)
        textFormat: Text.PlainText
        color: Theme.muted
        font.pixelSize: Theme.smallSize
        wrapMode: Text.Wrap
    }
    Repeater {
        model: graph.values.lines
        delegate: Text {
            required property var modelData
            required property int index
            width: graph.width
            text: "● " + modelData.name + ": " + modelData.latest + " " + graph.values.unit
            textFormat: Text.PlainText
            color: graph.colors[index % graph.colors.length]
            font.pixelSize: Theme.smallSize
            wrapMode: Text.Wrap
        }
    }
}
