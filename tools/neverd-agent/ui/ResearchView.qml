import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ColumnLayout {
    id: view
    required property var backend
    property var plan: ({})
    spacing: 10
    function refresh() { plan = backend.researchPlan(scope.currentIndex === 1) }
    ChoiceField {
        id: scope; Layout.fillWidth: true
        model: view.backend.sideChat ? [qsTr("主对话"), qsTr("当前侧边聊天")] : [qsTr("主对话")]
        onActivated: view.refresh()
    }
    ScrollView {
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
        ColumnLayout {
            width: parent.width; spacing: 14
            Label { visible: scope.currentIndex === 0 && !!view.backend.goal.objective; text: qsTr("持续目标"); color: Theme.secondary }
            Label { visible: scope.currentIndex === 0; text: view.backend.goal.objective || ""; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: qsTr("研究步骤"); color: Theme.secondary }
            Label { visible: !(view.plan.steps || []).length; text: qsTr("复杂分析可以让助手列出步骤，并随着分析更新。步骤不会自动产生新的模型请求。"); color: Theme.quiet; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Repeater {
                model: view.plan.steps || []
                delegate: Surface {
                    id: step; required property var modelData
                    Layout.fillWidth: true; implicitHeight: body.implicitHeight + 24; radius: 10
                    ColumnLayout {
                        id: body; anchors.fill: parent; anchors.margins: 12; spacing: 7
                        Label { text: ({pending:qsTr("待开始"),in_progress:qsTr("进行中"),completed:qsTr("已完成"),blocked:qsTr("待解决")}[step.modelData.status] || "") + " · " + step.modelData.title; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        Label { visible: !!step.modelData.summary; text: step.modelData.summary; color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        Repeater { model: step.modelData.evidence_ids; ActionButton { required property string modelData; text: qsTr("证据 · ") + modelData.slice(0, 8); onClicked: view.backend.openEvidence(modelData) } }
                        Label { visible: step.modelData.status === "completed" && !step.modelData.evidence_ids.length; text: qsTr("此步骤未附 ND 证据"); color: Theme.quiet; font.pointSize: Theme.smallPointSize }
                    }
                }
            }
            Label { text: qsTr("待确认的问题"); color: Theme.secondary }
            Label { visible: !(view.plan.questions || []).length; text: qsTr("暂无记录"); color: Theme.quiet }
            Repeater { model: view.plan.questions || []; Label { required property string modelData; text: "• " + modelData; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true } }
        }
    }
    Connections {
        target: view.backend
        function onReviewChanged() { view.refresh() }
        function onSideChatChanged() { view.refresh() }
        function onWorkspaceChanged() { view.refresh() }
    }
    Component.onCompleted: refresh()
}
