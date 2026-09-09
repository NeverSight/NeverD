pragma Singleton
import QtQuick

QtObject {
    readonly property color editor: "#1e1e1e"
    readonly property color sidebar: "#252526"
    readonly property color elevated: "#2d2d2d"
    readonly property color toolbar: "#333333"
    readonly property color hover: "#37373d"
    readonly property color border: "#3c3c3c"
    readonly property color foreground: "#d4d4d4"
    readonly property color muted: "#a7a7a7"
    readonly property color subdued: "#858585"
    readonly property color accent: "#007acc"
    readonly property color selection: "#04395e"
    readonly property color focus: "#007fd4"
    readonly property color accentForeground: "#ffffff"
    readonly property color accentForegroundMuted: "#70b4e3"
    readonly property color accentTrack: "#409bd7"
    readonly property color functionName: "#dcdcaa"
    readonly property color typeName: "#4ec9b0"
    readonly property color variable: "#9cdcfe"
    readonly property color keyword: "#569cd6"
    readonly property color controlFlow: "#c586c0"
    readonly property color stringValue: "#ce9178"
    readonly property color number: "#b5cea8"
    readonly property color comment: "#6a9955"
    readonly property color error: "#f48771"
    readonly property color warning: "#cca700"
    readonly property string monoFont: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "DejaVu Sans Mono"
    readonly property real captionSize: 9
    readonly property real bodySize: 10
    readonly property real codeSize: 11
    readonly property real titleSize: 15
}
