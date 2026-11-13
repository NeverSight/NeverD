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
        Label { text: qsTr("待发送 %1 条").arg(queue.inputs.length) + (queue.paused ? qsTr(" · 已暂停") : ""); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
        ActionButton { visible: !queue.active; text: qsTr("继续队列"); implicitHeight: 26; onClicked: queue.resumeRequested() }
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
            ActionButton { glyph: "close_small"; hint: qsTr("移出队列"); implicitHeight: 24; onClicked: queue.discardRequested(modelData.id) }
        }
    }
}
