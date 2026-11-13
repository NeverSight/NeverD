import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Dialog {
    id: picker
    required property var backend
    property bool side: false
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(560, parent.width - 48)
    modal: true; padding: 18
    background: Surface { glass: true }
    header: Label { text: qsTr("添加分析上下文"); color: Theme.text; padding: 18; font.pointSize: Theme.bodyPointSize; font.weight: Font.Medium }
    contentItem: ColumnLayout {
        spacing: 12
        Label { text: qsTr("引用当前项目的对象、函数或地址。发送前可以查看和移除引用。"); color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
        ChoiceField { id: objects; Layout.fillWidth: true; model: picker.backend.activeProject.objects || []; textRole: "name"; valueRole: "id" }
        Field { id: address; Layout.fillWidth: true; placeholderText: qsTr("函数名或地址（留空引用整个对象）") }
        ActionButton {
            text: qsTr("添加对象 / 函数引用"); primary: true
            enabled: !!objects.currentValue
            onClicked: { picker.backend.attachObjectReference(objects.currentValue, address.text, picker.side); picker.close() }
        }
        ActionButton {
            visible: !!picker.backend.evidence.id
            text: qsTr("引用当前查看的证据")
            onClicked: { picker.backend.attachEvidence(picker.backend.evidence.id, picker.side); picker.close() }
        }
        ActionButton {
            visible: picker.backend.previewTitle.length > 0
            text: qsTr("附带当前文件内容：") + picker.backend.previewTitle
            onClicked: { picker.backend.attachPreview(picker.side); picker.close() }
        }
        Label { text: qsTr("也可以选中对话或伪代码中的文字，通过右键引用或发起侧边聊天。"); color: Theme.quiet; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pointSize: Theme.smallPointSize }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ActionButton { text: qsTr("关闭"); onClicked: picker.close() }
        }
    }
}
