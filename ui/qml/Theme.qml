pragma Singleton
import QtQuick

// The page's colours and sizes, for dark and for light: the variables at the top of
// app/qkvm/static/app.css. When the page changes one, it changes here.
QtObject {
    // Dark or light: as the program is told to look (ui/look.h, which tells Qt, which tells this),
    // and as Windows is set where it is told nothing. (Qt's platform without a screen takes no
    // telling: there a test tells it here, as the program's look tells it.)
    property int scheme: Application.styleHints.colorScheme
    readonly property bool dark: scheme !== Qt.ColorScheme.Light

    readonly property color bg: dark ? "#080b11" : "#edf0f5"
    readonly property color bgGlow: dark ? Qt.rgba(90 / 255, 169 / 255, 1, 0.09) : Qt.rgba(31 / 255, 102 / 255, 209 / 255, 0.1)
    readonly property color panel: dark ? "#1e293b" : "#ffffff"
    readonly property color inset: dark ? "#0f172a" : "#f8fafc"
    readonly property color hover: dark ? "#334155" : "#eff6ff"
    readonly property color line: dark ? Qt.rgba(203 / 255, 213 / 255, 225 / 255, 0.16) : Qt.rgba(30 / 255, 41 / 255, 59 / 255, 0.16)
    readonly property color lineStrong: dark ? Qt.rgba(203 / 255, 213 / 255, 225 / 255, 0.24) : Qt.rgba(30 / 255, 41 / 255, 59 / 255, 0.3)
    readonly property color text: dark ? "#f8fafc" : "#0f172a"
    readonly property color muted: dark ? "#94a3b8" : "#64748b"
    readonly property color accent: "#1d4ed8"
    readonly property color accentHover: "#1e40af"
    readonly property color accentActive: "#1e3a8a"
    readonly property color accentLine: dark ? "#2563eb" : Qt.rgba(30 / 255, 58 / 255, 138 / 255, 0.6)
    readonly property color accentText: dark ? "#60a5fa" : "#1d4ed8"
    readonly property color accentSoft: dark ? Qt.rgba(59 / 255, 130 / 255, 246 / 255, 0.14) : Qt.rgba(29 / 255, 78 / 255, 216 / 255, 0.08)
    readonly property color textOnAccent: "#ffffff"
    readonly property color ok: dark ? "#22c55e" : "#16a34a"
    readonly property color warn: dark ? "#f59e0b" : "#d97706"
    readonly property color bad: dark ? "#ef4444" : "#dc2626"
    readonly property color badSoft: dark ? Qt.rgba(239 / 255, 68 / 255, 68 / 255, 0.12) : Qt.rgba(220 / 255, 38 / 255, 38 / 255, 0.07)
    // What lies over the window behind a dialog.
    readonly property color scrim: Qt.rgba(2 / 255, 6 / 255, 23 / 255, 0.6)
    // The inside of a frame of lines in a section of the settings: the inset colour, a little of it
    // over the panel's (the page's ".lines").
    readonly property color lines: Qt.rgba(inset.r * 0.38 + panel.r * 0.62, inset.g * 0.38 + panel.g * 0.62, inset.b * 0.38 + panel.b * 0.62, 1)

    // A machine's screen and what stands in for one keep these in both themes: they are video, not
    // the program's own surface.
    readonly property color screen: "#020617"
    readonly property color screenVeil: Qt.rgba(2 / 255, 6 / 255, 23 / 255, 0.86)
    readonly property color screenText: "#f8fafc"
    readonly property color screenMuted: "#94a3b8"
    readonly property color screenFaint: "#64748b"
    readonly property color screenLine: Qt.rgba(203 / 255, 213 / 255, 225 / 255, 0.3)
    readonly property color screenPanel: Qt.rgba(15 / 255, 23 / 255, 42 / 255, 0.88)
    readonly property color screenOk: "#4ade80"
    readonly property color screenWarn: "#fbbf24"
    readonly property color screenBad: "#f87171"

    // One corner, one height: every control has the same shape.
    readonly property int radius: 4
    readonly property int controlHeight: 32
    readonly property int barHeight: 36
    readonly property int gap: 12

    // A window narrower than this is a narrow one: the settings box lists its sections in a row
    // above the section, and each line of a section has its right-hand side under its left (the
    // page's 760 for its box, and the 12 at either side of it).
    readonly property int narrowWindow: 784

    readonly property int textSize: 13
    readonly property int smallSize: 12
    readonly property int titleSize: 15
    readonly property int headingSize: 11

    // Milliseconds. Nothing moves when Windows is set to show no animations.
    readonly property bool still: Application.styleHints.accessibility.motionPreference === Qt.MotionPreference.ReducedMotion
    readonly property int fast: still ? 0 : 150
    readonly property int medium: still ? 0 : 220

    // The colour of a state's dot and of the line along a card's top, by the server's word for the
    // state; nothing for a machine that is simply live.
    function stateColor(state: string): color {
        if (state === "online")
            return ok;
        if (state === "no_signal" || state === "busy" || state === "connecting")
            return warn;
        if (state === "offline" || state === "auth" || state === "error")
            return bad;
        return muted;
    }
}
