import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ColumnLayout {
    id: view
    required property var backend
    spacing: 10
    Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondary
        text: view.backend.reviewBusy ? qsTr("正在核对并保存 ND 变更…") : qsTr("先核对变更和证据，再应用到 ND 工程。原始二进制保持不变。")
    }
    ScrollView {
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
        ColumnLayout {
            width: parent.width; spacing: 14
            Label { visible: view.backend.changes.length === 0; text: qsTr("还没有变更提案。\n分析时可以要求命名、添加注释或完善类型。"); color: Theme.quiet; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Repeater {
                model: view.backend.changes
                delegate: Surface {
                    id: card
                    required property var modelData
                    Layout.fillWidth: true; implicitHeight: content.implicitHeight + 28
                    radius: Theme.radius
                    ColumnLayout {
                        id: content
                        anchors.fill: parent; anchors.margins: 14; spacing: 8
                        Label { text: card.modelData.objectName + " · " + (card.modelData.address || qsTr("本地类型")); color: Theme.text; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                        Label {
                            text: ({rename:qsTr("命名"),comment:qsTr("注释"),function_type:qsTr("函数类型"),types:qsTr("类型声明")}[card.modelData.kind] || card.modelData.kind) + " · " +
                                  ({pending:qsTr("待审阅"),applied:qsTr("已应用"),undone:qsTr("已撤销"),dismissed:qsTr("已忽略"),conflict:qsTr("当前值已变化"),failed:qsTr("失败"),uncertain:qsTr("写入结果待确认"),applied_unverified:qsTr("已保存，回执未确认"),no_change:qsTr("没有新增变更")}[card.modelData.status] || card.modelData.status)
                            color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
                        }
                        Label { text: card.modelData.rationale; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        Label { text: qsTr("修改前"); color: Theme.quiet }
                        MarkdownView { source: card.modelData.beforeDisplay || qsTr("（空）"); markdown: false; Layout.fillWidth: true; font.family: "JetBrains Mono NL" }
                        Label { text: qsTr("修改后"); color: Theme.quiet }
                        MarkdownView { source: card.modelData.after || qsTr("（清除）"); markdown: false; Layout.fillWidth: true; font.family: "JetBrains Mono NL" }
                        Repeater {
                            model: card.modelData.evidenceIds
                            ActionButton { required property string modelData; text: qsTr("查看依据 · ") + modelData.slice(0, 8); onClicked: view.backend.openEvidence(modelData) }
                        }
                        Label { visible: !!card.modelData.error; text: card.modelData.error || ""; color: Theme.error; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                        Flow {
                            Layout.fillWidth: true; spacing: 6
                            ActionButton { visible: card.modelData.status === "pending" || card.modelData.status === "failed"; text: qsTr("应用变更"); enabled: !view.backend.reviewBusy && !view.backend.running; onClicked: view.backend.applyChange(card.modelData.id) }
                            ActionButton { visible: ["pending","failed","conflict"].includes(card.modelData.status); text: qsTr("忽略"); enabled: !view.backend.reviewBusy; onClicked: view.backend.dismissChange(card.modelData.id) }
                            ActionButton { visible: card.modelData.status === "applied"; text: qsTr("撤销此变更"); enabled: !view.backend.reviewBusy && !view.backend.running; onClicked: view.backend.applyChange(card.modelData.id, true) }
                        }
                    }
                }
            }
        }
    }
    ActionButton { text: qsTr("刷新"); glyph: "refresh"; onClicked: view.backend.refreshResearch() }
}
