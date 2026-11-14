import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ToolButton {
    id: meter
    property var usage: ({})
    property bool compact: false
    property bool pinned: false
    readonly property bool known: usage.tokens !== undefined
    readonly property real fraction: known && usage.trigger > 0 ? usage.tokens / usage.trigger : 0
    readonly property real remaining: Math.max(0, (usage.trigger || 0) - (usage.tokens || 0))
    function number(value) { return Number(value || 0).toLocaleString(Qt.locale(), 'f', 0) }
    function compactNumber(value) {
        const v = Number(value || 0)
        return v >= 1e6 ? (v / 1e6).toFixed(2) + "M" : v >= 1e3 ? (v / 1e3).toFixed(1) + "k" : String(v)
    }
    implicitWidth: 32
    implicitHeight: 32
    hoverEnabled: true
    padding: 5
    text: usage.compacting ? qsTr("正在压缩上下文") : known ? qsTr("当前上下文 %1 tokens，距压缩还剩 %2").arg(number(usage.tokens)).arg(number(remaining)) : qsTr("上下文 · 待统计")
    Accessible.name: text
    font.pointSize: Theme.smallPointSize
    palette.buttonText: Theme.secondary
    background: Rectangle { color: meter.hovered ? Theme.toolHover : "transparent"; radius: Theme.toolRadius }
    contentItem: Canvas {
        id: ring
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const radius = Math.max(0, Math.min(width, height) / 2 - 3)
            ctx.lineWidth = 2.5
            ctx.strokeStyle = Theme.checkBoxBorder
            ctx.beginPath(); ctx.arc(width / 2, height / 2, radius, 0, Math.PI * 2); ctx.stroke()
            if (meter.known && meter.fraction > 0) {
                ctx.strokeStyle = meter.fraction >= 1 ? Theme.error : Theme.secondary
                ctx.lineCap = "round"
                ctx.beginPath(); ctx.arc(width / 2, height / 2, radius, -Math.PI / 2, -Math.PI / 2 + Math.min(1, meter.fraction) * Math.PI * 2); ctx.stroke()
            }
        }
        Connections { target: meter; function onUsageChanged() { ring.requestPaint() } }
        Connections { target: Theme; function onLightChanged() { ring.requestPaint() } }
    }
    onHoveredChanged: {
        if (hovered) { closeTimer.stop(); hoverTimer.restart() }
        else { hoverTimer.stop(); closeTimer.restart() }
    }
    onClicked: { pinned = !pinned; if (pinned) details.open(); else details.close() }
    Timer { id: hoverTimer; interval: 250; onTriggered: details.open() }
    Timer { id: closeTimer; interval: 180; onTriggered: { if (!meter.pinned && !meter.hovered && !popupHover.hovered) details.close() } }
    Popup {
        id: details
        parent: Overlay.overlay
        width: 236
        height: contentItem.implicitHeight + topPadding + bottomPadding
        // mapToItem is not reactive; place the card each time it opens.
        onAboutToShow: {
            const at = meter.mapToItem(parent, 0, 0)
            x = Math.max(8, Math.min(parent.width - width - 8, at.x + meter.width - width))
            y = Math.max(8, at.y - implicitHeight - 6)
        }
        padding: 12
        onClosed: meter.pinned = false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Surface { glass: true; radius: Theme.radius }
        contentItem: ColumnLayout {
            spacing: 8
            HoverHandler { id: popupHover; onHoveredChanged: { if (!hovered) closeTimer.restart() } }
            RowLayout {
                Layout.fillWidth: true
                Label { text: meter.usage.compacting ? qsTr("正在压缩") : qsTr("上下文"); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
                Label { visible: meter.known; text: Math.round(meter.fraction * 100) + "%"; color: meter.fraction >= 1 ? Theme.error : Theme.text; font.pointSize: Theme.smallPointSize; font.weight: Font.Medium }
            }
            Rectangle {
                visible: meter.known
                Layout.fillWidth: true; implicitHeight: 3; radius: 1.5; color: Theme.raised
                Rectangle { width: parent.width * Math.min(1, meter.fraction); height: parent.height; radius: 1.5; color: meter.fraction >= 1 ? Theme.error : Theme.accent }
            }
            Label {
                Layout.fillWidth: true
                text: meter.known ? qsTr("%1 / %2 tokens").arg(meter.compactNumber(meter.usage.tokens)).arg(meter.compactNumber(meter.usage.trigger)) + (meter.usage.estimated ? qsTr(" · 估算") : "") : qsTr("首次请求后统计")
                color: Theme.secondary; font.pointSize: Theme.smallPointSize
            }
            Label {
                visible: (meter.usage.compactions || 0) > 0
                Layout.fillWidth: true
                text: qsTr("已压缩 %1 次").arg(meter.usage.compactions || 0)
                color: Theme.quiet; font.pointSize: Theme.smallPointSize
            }
        }
    }
}
