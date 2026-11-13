import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Dialog {
    id: editor
    required property var backend
    property bool editing: false
    anchors.centerIn: parent
    modal: true
    width: Math.min(640, parent.width - 48)
    height: Math.min(660, parent.height - 48)
    padding: 18
    background: Surface { glass: true; radius: Theme.workspaceRadius }
    header: Label { text: qsTr("持续目标"); color: Theme.text; font.pointSize: Theme.bodyPointSize; font.weight: Font.Medium; padding: 18 }
    onOpened: {
        const goal = backend.goal
        editing = !!goal.id && goal.status !== "completed"
        objective.text = goal.objective || ""
        tokens.text = String(goal.tokenBudget || goal.defaultTokenBudget)
        rounds.text = String(goal.roundLimit || goal.defaultRoundLimit)
    }
    contentItem: ScrollView {
        clip: true; contentWidth: availableWidth
        ColumnLayout {
            width: parent.width; spacing: 12
            Label { text: qsTr("让 Agent 围绕一个目标持续推进，直到完成、暂停或达到上限。"); color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
            ScrollView {
                Layout.fillWidth: true; Layout.preferredHeight: 135; clip: true
                TextArea {
                    id: objective
                    enabled: !editor.backend.running
                    placeholderText: qsTr("描述预期成果和完成条件…")
                    placeholderTextColor: Theme.quiet; color: Theme.text
                    wrapMode: TextEdit.Wrap; selectByMouse: true; padding: 12
                    background: Rectangle { color: Theme.raised; radius: 8; border.color: objective.activeFocus ? Theme.focusBorder : Theme.border }
                }
            }
            GridLayout {
                columns: 2; Layout.fillWidth: true; columnSpacing: 16
                Label { text: qsTr("累计 token 预算"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("最多推进轮数"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: tokens; enabled: !editor.backend.running; validator: IntValidator { bottom: 1 } Layout.fillWidth: true }
                Field { id: rounds; enabled: !editor.backend.running; validator: IntValidator { bottom: 1 } Layout.fillWidth: true }
            }
            Label { text: qsTr("预算包含输入、输出、子任务和上下文压缩。请求前按估算预留，实际用量以服务端计量为准。继续目标会保留已用预算。"); color: Theme.quiet; font.pointSize: Theme.smallPointSize; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { visible: !!editor.backend.goal.summary; text: qsTr("当前进展"); color: Theme.text; font.weight: Font.Medium }
            MarkdownView { visible: !!editor.backend.goal.summary; source: editor.backend.goal.summary || ""; Layout.fillWidth: true; onLinkRequested: link => editor.backend.openTextLink(link) }
            Label { visible: !!editor.backend.goal.evidence; text: qsTr("已记录的证据"); color: Theme.text; font.weight: Font.Medium }
            MarkdownView { visible: !!editor.backend.goal.evidence; source: editor.backend.goal.evidence || ""; Layout.fillWidth: true; onLinkRequested: link => editor.backend.openTextLink(link) }
            Label { visible: !!editor.backend.goal.nextStep; text: qsTr("下一步：") + editor.backend.goal.nextStep; color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { visible: !!editor.backend.goal.error; text: editor.backend.goal.error || ""; color: Theme.error; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
        }
    }
    footer: RowLayout {
        spacing: 8
        ActionButton {
            Layout.leftMargin: 18; Layout.bottomMargin: 18
            visible: editor.backend.goal.status === "active"
            text: qsTr("暂停目标"); onClicked: editor.backend.pauseGoal()
        }
        Item { Layout.fillWidth: true }
        ActionButton { text: qsTr("关闭"); Layout.bottomMargin: 18; onClicked: editor.close() }
        ActionButton {
            text: editor.editing ? qsTr("保存并继续") : qsTr("开始目标")
            primary: true; Layout.bottomMargin: 18; Layout.rightMargin: 18
            enabled: !editor.backend.running && objective.text.trim().length > 0 && tokens.acceptableInput && rounds.acceptableInput
            onClicked: {
                const ok = editor.editing
                    ? editor.backend.updateGoal(objective.text, Number(tokens.text), Number(rounds.text)) && editor.backend.resumeGoal()
                    : editor.backend.startGoal(objective.text, Number(tokens.text), Number(rounds.text))
                if (ok) editor.close()
            }
        }
    }
}
