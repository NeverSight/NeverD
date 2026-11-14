import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ColumnLayout {
    id: queue
    property var inputs: []
    property bool active: false
    property bool paused: false
    signal discardRequested(string inputId)
    signal resumeRequested()
    visible: inputs.length > 0
    spacing: 4
    RowLayout {
        Layout.fillWidth: true
        Label { text: (queue.paused ? qsTr("Queued: %1 · Paused") : qsTr("Queued: %1")).arg(queue.inputs.length); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
        ActionButton { visible: !queue.active; text: qsTr("Resume queue"); implicitHeight: 26; onClicked: queue.resumeRequested() }
    }
    ListView {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(3, queue.inputs.length) * 32
        clip: true; model: queue.inputs
        ScrollBar.vertical: ThemedScrollBar {}
        delegate: RowLayout {
            required property var modelData
            width: ListView.view.width; height: 32; spacing: 8
            Label { text: modelData.text; elide: Text.ElideRight; color: Theme.secondary; Layout.fillWidth: true; font.pointSize: Theme.smallPointSize }
            ActionButton { glyph: "close_small"; hint: qsTr("Remove from queue"); implicitHeight: 24; onClicked: queue.discardRequested(modelData.id) }
        }
    }
}
