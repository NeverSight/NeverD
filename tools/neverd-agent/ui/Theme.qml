pragma Singleton
import QtQuick

QtObject {
    property bool light: false
    property Item backdrop: null
    property real basePointSize: 10.5
    readonly property int controlHeight: 32
    readonly property int controlRadius: 8
    readonly property int iconSize: 17
    readonly property int spaceSmall: 8
    readonly property int spaceMedium: 14
    readonly property int spaceLarge: 22
    // Flat console palette: near-black ground, cards separated by hairline borders,
    // and a single muted light blue reserved for state (send, selection, focus, links).
    readonly property color background: light ? "#eceef2" : "#0a0b0e"
    readonly property color surface: light ? "#f7f8fa" : "#111318"
    readonly property color content: light ? "#fbfcfd" : "#0e1014"
    readonly property color raised: light ? "#eef0f4" : "#181b21"
    readonly property color userBubble: light ? "#eef1f5" : "#171a20"
    readonly property color assistantBubble: "transparent"
    readonly property color bubbleBorder: light ? "#16000000" : "#1affffff"
    readonly property color code: light ? "#eef0f4" : "#0b0c10"
    readonly property color hover: light ? "#0d000000" : "#0dffffff"
    readonly property color selected: light ? "#17000000" : "#16ffffff"
    readonly property color border: light ? "#16000000" : "#14ffffff"
    readonly property color borderStrong: light ? "#24000000" : "#22ffffff"
    readonly property color shadow: light ? "#0f1722" : "#020408"
    readonly property color text: light ? "#151820" : "#e8ebf0"
    readonly property color secondary: light ? "#5a6170" : "#99a1ad"
    readonly property color quiet: light ? "#7a8190" : "#6c7482"
    readonly property color composerTint: light ? "#ffffff" : "#13161b"
    readonly property color focusBorder: light ? "#7aa3d0" : "#4a78ad"
    readonly property color accent: light ? "#2f6fb0" : "#8bb8ea"
    readonly property color accentSoft: light ? "#142f6fb0" : "#1a8bb8ea"
    readonly property color accentLine: light ? "#802f6fb0" : "#998bb8ea"
    readonly property color focus: accent
    readonly property color primary: light ? "#2f6fb0" : "#78a9e0"
    readonly property color primaryText: light ? "#ffffff" : "#08111e"
    readonly property color menu: light ? "#ffffff" : "#15181e"
    readonly property color titleBar: windowFrame.chromeColor("TitleBar", light)
    readonly property color titleText: windowFrame.chromeColor("TitleBarText", light)
    readonly property color titleInactiveText: windowFrame.chromeColor("TitleBarInactiveText", light)
    readonly property color controlHover: windowFrame.chromeColor("WindowControlHover", light)
    readonly property color controlPressed: windowFrame.chromeColor("WindowControlPressed", light)
    readonly property color closeHover: windowFrame.chromeColor("WindowCloseHover", light)
    readonly property color closePressed: windowFrame.chromeColor("WindowClosePressed", light)
    readonly property color closeText: windowFrame.chromeColor("WindowCloseText", light)
    readonly property color error: light ? "#b03a3a" : "#e58a8a"
    readonly property real smallPointSize: basePointSize - 0.5
    readonly property real bodyPointSize: basePointSize + 0.5
    readonly property real titlePointSize: basePointSize + 4
    readonly property real radius: 10
    readonly property real workspaceRadius: 12
}
