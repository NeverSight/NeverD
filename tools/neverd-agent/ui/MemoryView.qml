import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ColumnLayout {
    id: view
    required property var backend
    signal addRequested(string objectId)
    signal editRequested(string objectId, string id, string text)
    spacing: 12

    Label {
        Layout.fillWidth: true
        text: view.backend.memoryStatus || qsTr("任务结束后在空闲时整理记忆；下次按问题召回。")
        color: Theme.secondary
        wrapMode: Text.Wrap
    }
    RowLayout {
        Layout.fillWidth: true
        Label {
            text: (view.backend.useMemory ? qsTr("召回已开启") : qsTr("召回已关闭")) + " · " +
                (view.backend.generateMemory ? qsTr("自动整理已开启") : qsTr("自动整理已关闭"))
            color: Theme.secondary
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }
        ActionButton {
            glyph: "refresh"; hint: qsTr("立即整理待处理记录；失败项重新尝试")
            enabled: view.backend.generateMemory && !view.backend.memoryWorking
            onClicked: view.backend.organizeMemory()
        }
    }
    component Entry: ColumnLayout {
        id: entry
        required property var record
        required property string scopeId
        property bool expanded: false
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: (entry.record.title || qsTr("手动记忆")) +
                (entry.record.state === "needs_review" ? qsTr(" · 冲突待确认") :
                 entry.record.state === "superseded" ? qsTr(" · 已被替代") : "")
            color: entry.record.state === "needs_review" ? Theme.error : Theme.text
            font.weight: Font.Medium
            wrapMode: Text.Wrap
        }
        TextEdit {
            Layout.fillWidth: true
            text: entry.record.text
            textFormat: TextEdit.PlainText
            readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
            color: entry.record.state === "superseded" ? Theme.secondary : Theme.text
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: entry.record.automatic ?
                    ({preference: qsTr("用户偏好"), procedure: qsTr("研究方法"), finding: qsTr("历史发现 · 需核验"), hypothesis: qsTr("推测")}[entry.record.category] || qsTr("自动提炼")) : qsTr("手动记录")
                color: Theme.secondary; Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            ActionButton {
                glyph: "comment"; hint: qsTr("编辑 / 确认此版本")
                onClicked: view.editRequested(entry.scopeId, entry.record.id, entry.record.text)
            }
            ActionButton {
                glyph: "close_small"; hint: qsTr("遗忘；阻止自动整理重新加入同一条目")
                onClicked: view.backend.forgetMemory(entry.scopeId, entry.record.id)
            }
        }
        ActionButton {
            visible: !!entry.record.source
            text: entry.expanded ? qsTr("收起来源") : qsTr("查看来源")
            onClicked: entry.expanded = !entry.expanded
        }
        ColumnLayout {
            visible: entry.expanded && !!entry.record.source
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: entry.record.source ? qsTr("原文 · ") + entry.record.source.createdAt : ""
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            TextEdit {
                Layout.fillWidth: true
                text: entry.record.source ? entry.record.source.quote : ""
                textFormat: TextEdit.PlainText
                readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                color: Theme.secondary
            }
            Label {
                text: entry.record.source ? qsTr("来源任务：") + entry.record.source.runId : ""
                color: Theme.secondary; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true
            }
            Repeater {
                model: entry.record.source ? entry.record.source.evidence : []
                delegate: ActionButton {
                    required property var modelData
                    text: modelData.tool + (modelData.address ? " · " + modelData.address : "")
                    glyph: "file"
                    enabled: entry.record.originProjectId === view.backend.activeId
                    hint: enabled ? qsTr("打开原始证据快照") : qsTr("证据保存在来源项目；此处保留引用与原文")
                    onClicked: view.backend.openEvidence(modelData.id)
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
    }
    ScrollView {
        Layout.fillWidth: true; Layout.fillHeight: true
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            width: parent.width
            spacing: 14
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("项目记忆"); color: Theme.secondary; Layout.fillWidth: true }
                ActionButton { glyph: "plus"; hint: qsTr("添加项目记忆"); onClicked: view.addRequested("") }
            }
            Label {
                visible: view.backend.projectMemory.length === 0
                text: qsTr("项目偏好、研究方法和跨对象线索会保存在这里。")
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Repeater {
                model: view.backend.projectMemory
                delegate: Entry { required property var modelData; record: modelData; scopeId: "" }
            }
            Label {
                visible: !!view.backend.selectedObjectId
                text: (view.backend.selectedObject.name || "") + qsTr(" · 对象记忆")
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Label {
                visible: !!view.backend.selectedObjectId && view.backend.memory.length === 0
                text: qsTr("尚无对象记忆。自动提炼的发现始终绑定这份文件的 SHA-256。")
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Repeater {
                model: view.backend.memory
                delegate: Entry { required property var modelData; record: modelData; scopeId: view.backend.selectedObjectId }
            }
            Label {
                text: view.backend.sharing ? qsTr("来自其他项目 · 只读") : qsTr("全局对象记忆共享已关闭")
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Repeater {
                model: view.backend.sharedMemory
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label { text: modelData.sourceProject; color: Theme.secondary }
                    TextEdit {
                        Layout.fillWidth: true
                        text: modelData.text; textFormat: TextEdit.PlainText
                        readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; color: Theme.text
                    }
                }
            }
        }
    }
}
