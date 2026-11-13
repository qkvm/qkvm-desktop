import QtQuick
import Qkvm

// Where one is in something that is longer than its place: a thin mark at its right edge, there
// only while not all of it is seen. Without it, nobody would know that there is more below.
Rectangle {
    id: mark

    required property Flickable flickable

    parent: flickable
    visible: flickable.contentHeight > flickable.height + 1
    x: flickable.width - width
    y: flickable.visibleArea.yPosition * flickable.height
    width: 3
    height: Math.max(16, flickable.visibleArea.heightRatio * flickable.height)
    radius: 1.5
    color: Theme.lineStrong
}
