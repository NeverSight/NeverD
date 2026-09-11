pragma Singleton
import QtQuick

QtObject {
    id: root
    readonly property color editor: "#1e1e1e"
    readonly property color sidebar: "#252526"
    readonly property color elevated: "#2d2d2d"
    readonly property color toolbar: "#333333"
    readonly property color hover: "#37373d"
    readonly property color border: "#3c3c3c"
    readonly property color foreground: "#d4d4d4"
    readonly property color muted: "#a7a7a7"
    readonly property color subdued: "#969696"
    readonly property color accent: "#007acc"
    readonly property color selection: "#04395e"
    readonly property color inactiveSelection: "#37373d"
    readonly property color accentHover: "#1689d1"
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
    readonly property string uiFont: Qt.application.font.family
    // Cocoa uses a 72-DPI point baseline. Keep native UI text readable there
    // while honoring larger system fonts on every desktop platform.
    readonly property real bodySize: Math.max(Qt.application.font.pointSize, Qt.platform.os === "osx" ? 13 : 10)
    readonly property real captionSize: Math.round(bodySize / 1.125)
    readonly property real titleSize: Math.round(bodySize * 1.125)
    readonly property real codeSize: Qt.platform.os === "osx" ? captionSize : Math.max(11, Math.round(bodySize))
    readonly property FontMetrics uiMetrics: FontMetrics { font.family: root.uiFont; font.pointSize: root.bodySize }
    readonly property FontMetrics captionMetrics: FontMetrics { font.family: root.uiFont; font.pointSize: root.captionSize }
    readonly property int controlHeight: Math.max(28, Math.ceil(uiMetrics.height) + 12)
    readonly property int compactControlHeight: Math.max(24, Math.ceil(uiMetrics.height) + 8)
    readonly property int tabHeight: Math.max(28, Math.ceil(uiMetrics.height) + 12)
    readonly property int dockTitleHeight: Math.max(26, Math.ceil(uiMetrics.height) + 10)
    readonly property int rowHeight: Math.max(24, Math.ceil(uiMetrics.height) + 8)
    readonly property int toolbarHeight: controlHeight + 12
    readonly property int contextHeight: Math.max(22, Math.ceil(captionMetrics.height) + 6)
    readonly property int codeGutter: 14
}
