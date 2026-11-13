import QtQuick
import QtQuick.Shapes
import Qkvm

// The soft glow at the top of the background, and nothing louder. It is the page's
// radial-gradient(1100px 460px at 50% -180px, glow, transparent 70%): a circle of that width,
// pressed flat to that height, with its middle above the window's top edge.
Item {
    id: glow

    readonly property real across: 1100
    readonly property real down: 460

    clip: true

    Shape {
        x: glow.width / 2 - glow.across
        y: -180 - glow.down
        width: 2 * glow.across
        height: 2 * glow.across
        transform: Scale {
            yScale: glow.down / glow.across
        }

        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: glow.across
                centerY: glow.across
                focalX: glow.across
                focalY: glow.across
                centerRadius: glow.across

                GradientStop {
                    position: 0
                    color: Theme.bgGlow
                }
                // The same colour with nothing of it left: fading towards plain "transparent" would
                // pass through grey.
                GradientStop {
                    position: 0.7
                    color: Qt.rgba(Theme.bgGlow.r, Theme.bgGlow.g, Theme.bgGlow.b, 0)
                }
            }
            startX: 0
            startY: 0

            PathLine {
                x: 2 * glow.across
                y: 0
            }
            PathLine {
                x: 2 * glow.across
                y: 2 * glow.across
            }
            PathLine {
                x: 0
                y: 2 * glow.across
            }
            PathLine {
                x: 0
                y: 0
            }
        }
    }
}
