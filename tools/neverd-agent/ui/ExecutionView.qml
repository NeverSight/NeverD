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
    // Opened, the record asks to be shown whole, once it has its rows.
    signal opened()
    property bool revealPending: false
    function refresh() {
        if (!expanded || !runId || !backend) return
        records = backend.execution(runId)
        if (revealPending) { revealPending = false; Qt.callLater(trace.opened) }
    }
    onExpandedChanged: { revealPending = expanded; refresh() }
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
                text: modelData.state === "queued" ? qsTr("Queued")
                    : modelData.state === "succeeded" ? qsTr("Concluded")
                    : modelData.state === "failed" ? qsTr("Not completed; see details")
                    : modelData.state === "cancelled" ? qsTr("Stopped")
                    : modelData.activity === "nd_query" ? qsTr("Querying NeverD")
                    : modelData.activity || qsTr("Preparing analysis")
                color: modelData.state === "failed" ? Theme.error : Theme.secondary
                elide: Text.ElideRight; font.pointSize: Theme.smallPointSize
            }
            Label {
                text: modelData.steps ? qsTr("Steps: %1").arg(modelData.steps) : ""
                color: Theme.quiet; font.pointSize: Theme.smallPointSize
            }
        }
    }
    ActionButton {
        Layout.fillWidth: true
        glyph: trace.expanded ? "chevron_down" : "chevron_right"
        text: trace.records.length ? (trace.active ? qsTr("Running · Show tool and object progress · %1") : qsTr("Execution · %1")).arg(trace.records.length)
              : trace.active ? qsTr("Running · Show tool and object progress") : qsTr("Execution")
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
                        text: ({running: qsTr("Running"), succeeded: qsTr("Done"), failed: qsTr("Failed"), cancelled: qsTr("Stopped"), skipped: qsTr("Skipped")}[row.modelData.state] || "") + (row.modelData.elapsedMs ? " · " + qsTr("%1 s").arg(Number(row.modelData.elapsedMs / 1000).toLocaleString(Qt.locale(), "f", 1)) : "")
                        color: row.modelData.state === "failed" ? Theme.error : Theme.quiet; font.pointSize: Theme.smallPointSize
                    }
                    ActionButton { quiet: true; text: row.detail ? qsTr("Hide details") : qsTr("Details"); implicitHeight: 24; onClicked: row.detail = !row.detail }
                    ActionButton { visible: !!row.modelData.evidenceId; quiet: true; text: qsTr("Evidence"); implicitHeight: 24; onClicked: trace.backend.openEvidence(row.modelData.evidenceId) }
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
        Label { visible: trace.records.length === 0; text: qsTr("No tool calls in this round yet."); color: Theme.quiet; font.pointSize: Theme.smallPointSize }
    }
}
