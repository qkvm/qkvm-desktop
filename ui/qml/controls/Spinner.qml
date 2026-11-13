import QtQuick
import QtQuick.Shapes
import Qkvm

// Something is under way: three quarters of a ring, turning. It stands still where Windows is set
// to show no animations.
Item {
    id: spinner

    property color color: Theme.accentText
    property real size: 14

    implicitWidth: size
    implicitHeight: size

    Shape {
        id: ring

        width: spinner.size
        height: spinner.size
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: spinner.color
            strokeWidth: 2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap

            PathAngleArc {
                centerX: spinner.size / 2
                centerY: spinner.size / 2
                radiusX: spinner.size / 2 - 1
                radiusY: spinner.size / 2 - 1
                startAngle: 0
                sweepAngle: 270
            }
        }

        RotationAnimator on rotation {
            running: !Theme.still && spinner.visible
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
        }
    }
}
