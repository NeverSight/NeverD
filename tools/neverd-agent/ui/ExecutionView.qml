import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ColumnLayout {
    id: trace
    required property var backend
    required property string runId
    property bool active: false
    property bool expanded: false
    property var records: []
    readonly property var analysts: trace.active && trace.backend
        ? trace.backend.progress.filter(entry => entry.runId === trace.runId && entry.depth > 0) : []
    spacing: 6
    function refresh() { if (expanded && runId && backend) records = backend.execution(runId) }
    onExpandedChanged: refresh()
    onActiveChanged: refresh()
    Timer { interval: 600; repeat: true; running: trace.expanded && trace.active; onTriggered: trace.refresh() }
    Repeater {
        model: trace.analysts
        RowLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: 10
            Icon { name: "file"; Layout.preferredWidth: 14; Layout.preferredHeight: 14; color: Theme.quiet }
            Label {
                text: modelData.label
                color: Theme.text; font.pointSize: Theme.smallPointSize
            }
            Label {
                Layout.fillWidth: true
                text: modelData.state === "queued" ? qsTr("等待中")
                    : modelData.state === "succeeded" ? qsTr("已有结论")
                    : modelData.state === "failed" ? qsTr("未完成，可查看详情")
                    : modelData.state === "cancelled" ? qsTr("已停止")
                    : modelData.activity === "nd_query" ? qsTr("正在查询 ND")
                    : modelData.activity || qsTr("准备分析")
                color: modelData.state === "failed" ? Theme.error : Theme.secondary
                elide: Text.ElideRight; font.pointSize: Theme.smallPointSize
            }
            Label {
                text: modelData.steps ? qsTr("%1 步").arg(modelData.steps) : ""
                color: Theme.quiet; font.pointSize: Theme.smallPointSize
            }
        }
    }
    ActionButton {
        Layout.fillWidth: true
        glyph: trace.expanded ? "chevron_down" : "chevron_right"
        text: (trace.active ? qsTr("执行中 · 查看工具与对象进度") : qsTr("执行记录")) + (trace.records.length ? qsTr(" · %1 项").arg(trace.records.length) : "")
        palette.buttonText: Theme.secondary; hoverEnabled: true
        font.pointSize: Theme.smallPointSize
        background: Rectangle { color: parent.hovered ? Theme.toolHover : "transparent"; radius: Theme.toolRadius }
        onClicked: trace.expanded = !trace.expanded
    }
    ColumnLayout {
        visible: trace.expanded
        Layout.fillWidth: true; spacing: 8
        Repeater {
            model: trace.records
            ColumnLayout {
                id: row
                required property var modelData
                property bool detail: false
                Layout.fillWidth: true; spacing: 4
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: (row.modelData.label || "") + " · " + (row.modelData.name || "") + (row.modelData.address ? " · " + row.modelData.address : "")
                        color: Theme.text; font.pointSize: Theme.smallPointSize
                    }
                    Label {
                        text: ({running: qsTr("进行中"), succeeded: qsTr("完成"), failed: qsTr("失败"), cancelled: qsTr("已停止"), skipped: qsTr("已跳过")}[row.modelData.state] || "") + (row.modelData.elapsedMs ? " · " + (row.modelData.elapsedMs / 1000).toFixed(1) + "s" : "")
                        color: row.modelData.state === "failed" ? Theme.error : Theme.quiet; font.pointSize: Theme.smallPointSize
                    }
                    ActionButton { text: row.detail ? qsTr("收起") : qsTr("详情"); implicitHeight: 24; onClicked: row.detail = !row.detail }
                    ActionButton { visible: !!row.modelData.evidenceId; text: qsTr("证据"); implicitHeight: 24; onClicked: trace.backend.openEvidence(row.modelData.evidenceId) }
                }
                Loader {
                    Layout.fillWidth: true; Layout.preferredHeight: active ? 180 : 0
                    active: row.detail
                    sourceComponent: ScrollView {
                        clip: true
                        TextArea {
                            text: JSON.stringify({arguments: row.modelData.arguments || {}, result: row.modelData.result || {}}, null, 2)
                            readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                            color: Theme.secondary; font.family: "JetBrains Mono NL"; font.pointSize: Theme.smallPointSize
                            background: Rectangle { color: Theme.code; radius: Theme.controlRadius; border.width: 1; border.color: Theme.border }
                        }
                    }
                }
            }
        }
        Label { visible: trace.records.length === 0; text: qsTr("本轮尚无工具调用。"); color: Theme.quiet; font.pointSize: Theme.smallPointSize }
    }
}
