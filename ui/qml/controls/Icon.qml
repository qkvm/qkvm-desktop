import QtQuick
import QtQuick.Shapes
import Qkvm

// One of the page's line icons, by the page's name for it: a drawing on a grid of 24, in whatever
// colour and size it is given. The shapes are the page's (the list ICONS in app/qkvm/static/app.js,
// the Feather set), each written as one path; a name that is not here draws nothing.
Item {
    id: icon

    property string name
    property color color: Theme.text
    property real size: 14

    readonly property var paths: ({
            "activity": "M22 12h-4l-3 9L9 3l-3 9H2",
            "zoomIn": "M19 11a8 8 0 1 1-16 0a8 8 0 1 1 16 0zM21 21l-4.35-4.35M11 8v6M8 11h6",
            "camera": "M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2zM16 13a4 4 0 1 1-8 0a4 4 0 1 1 8 0z",
            "alert": "M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0zM12 9v4M12 17h.01",
            "archive": "M21 8v13H3V8M1 3h22v5H1zM10 12h4",
            "check": "M20 6L9 17l-5-5",
            "checkSquare": "M9 11l3 3L22 4M21 12v7a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11",
            "chevronDown": "M6 9l6 6 6-6",
            "chevronLeft": "M15 18l-6-6 6-6",
            "chevronRight": "M9 18l6-6-6-6",
            "clock": "M2 12a10 10 0 1 0 20 0a10 10 0 1 0-20 0M12 6v6l4 2",
            "coffee": "M18 8h1a4 4 0 0 1 0 8h-1M2 8h16v9a4 4 0 0 1-4 4H6a4 4 0 0 1-4-4V8zM6 1v3M10 1v3M14 1v3",
            "columns": "M12 3h7a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-7m0-18H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h7m0-18v18",
            "command": "M18 3a3 3 0 0 0-3 3v12a3 3 0 0 0 3 3 3 3 0 0 0 3-3 3 3 0 0 0-3-3H6a3 3 0 0 0-3 3 3 3 0 0 0 3 3 3 3 0 0 0 3-3V6a3 3 0 0 0-3-3 3 3 0 0 0-3 3 3 3 0 0 0 3 3h12a3 3 0 0 0 3-3 3 3 0 0 0-3-3z",
            "disc": "M2 12a10 10 0 1 0 20 0a10 10 0 1 0-20 0M9 12a3 3 0 1 0 6 0a3 3 0 1 0-6 0",
            "download": "M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3",
            "eject": "M5 15h14L12 6zM5 19h14",
            "enter": "M9 10l-5 5 5 5M20 4v7a4 4 0 0 1-4 4H4",
            "external": "M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6M15 3h6v6M10 14L21 3",
            "eye": "M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8zM9 12a3 3 0 1 0 6 0a3 3 0 1 0-6 0",
            "eyeOff": "M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24M1 1l22 22",
            "grid": "M4 3h5a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-5a1 1 0 0 1-1-1v-5a1 1 0 0 1 1-1zM15 3h5a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-5a1 1 0 0 1-1-1v-5a1 1 0 0 1 1-1zM4 14h5a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-5a1 1 0 0 1-1-1v-5a1 1 0 0 1 1-1zM15 14h5a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-5a1 1 0 0 1-1-1v-5a1 1 0 0 1 1-1z",
            "info": "M2 12a10 10 0 1 0 20 0a10 10 0 1 0-20 0M12 16v-4M12 8h.01",
            "list": "M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01",
            "lock": "M5 11h14a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2h-14a2 2 0 0 1-2-2v-7a2 2 0 0 1 2-2zM7 11V7a5 5 0 0 1 10 0v4",
            "logOut": "M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4M16 17l5-5-5-5M21 12H9",
            "maximize": "M8 3H5a2 2 0 0 0-2 2v3m18 0V5a2 2 0 0 0-2-2h-3m0 18h3a2 2 0 0 0 2-2v-3M3 16v3a2 2 0 0 0 2 2h3",
            "minimize": "M8 3v3a2 2 0 0 1-2 2H3m18 0h-3a2 2 0 0 1-2-2V3m0 18v-3a2 2 0 0 1 2-2h3M3 16h3a2 2 0 0 1 2 2v3",
            "moon": "M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z",
            "more": "M4 12a1 1 0 1 0 2 0a1 1 0 1 0-2 0M11 12a1 1 0 1 0 2 0a1 1 0 1 0-2 0M18 12a1 1 0 1 0 2 0a1 1 0 1 0-2 0",
            "pause": "M6 4h4v16H6zM14 4h4v16h-4z",
            "plus": "M12 5v14M5 12h14",
            "power": "M18.36 6.64a9 9 0 1 1-12.73 0M12 2v10",
            "rotate": "M23 4v6h-6M20.49 15a9 9 0 1 1-2.12-9.36L23 10",
            "search": "M3 11a8 8 0 1 0 16 0a8 8 0 1 0-16 0M21 21l-4.35-4.35",
            "sliders": "M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6",
            "star": "M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2z",
            "sun": "M8 12a4 4 0 1 0 8 0a4 4 0 1 0-8 0M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41",
            "system": "M4 3h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2h-16a2 2 0 0 1-2-2v-10a2 2 0 0 1 2-2zM8 21h8M12 17v4",
            "trash": "M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14",
            "tv": "M4 7h16a2 2 0 0 1 2 2v11a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V9a2 2 0 0 1 2-2zM17 2l-5 5-5-5",
            "type": "M4 7V4h16v3M9 20h6M12 4v16",
            "upload": "M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M17 8l-5-5-5 5M12 3v12",
            "users": "M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2M23 21v-2a4 4 0 0 0-3-3.87M16 3.13a4 4 0 0 1 0 7.75M5 7a4 4 0 1 0 8 0a4 4 0 1 0-8 0",
            "wifiOff": "M1 1l22 22M16.72 11.06A10.94 10.94 0 0 1 19 12.55M5 12.55a10.94 10.94 0 0 1 5.17-2.39M10.71 5.05A16 16 0 0 1 22.58 9M1.42 9a15.91 15.91 0 0 1 4.7-2.88M8.53 16.11a6 6 0 0 1 6.95 0M12 20h.01",
            "x": "M18 6L6 18M6 6l12 12",
            "zap": "M13 2L3 14h9l-1 8 10-12h-9l1-8z"
        })

    implicitWidth: size
    implicitHeight: size

    Shape {
        width: 24
        height: 24
        scale: icon.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: icon.color
            strokeWidth: 2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathSvg {
                path: icon.paths[icon.name] ?? ""
            }
        }
    }
}
