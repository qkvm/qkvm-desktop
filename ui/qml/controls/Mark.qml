pragma ComponentBehavior: Bound

import QtQuick

// The same official qKVM mark used for the window and notification area.
Item {
    id: mark

    property real size: 22

    implicitWidth: size
    implicitHeight: size

    Image {
        anchors.fill: parent
        source: "qrc:/qkvm/brand/mark.svg"
        sourceSize.width: Math.ceil(width * Screen.devicePixelRatio)
        sourceSize.height: Math.ceil(height * Screen.devicePixelRatio)
        fillMode: Image.PreserveAspectFit
    }
}
