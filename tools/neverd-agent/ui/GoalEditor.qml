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
    Overlay.modal: ModalDim {}
    height: Math.min(660, parent.height - 48)
    padding: 18
    background: Surface { glass: true; radius: Theme.radius; color: Theme.background }
    header: Label { text: qsTr("Persistent goal"); color: Theme.text; font.pointSize: Theme.bodyPointSize; font.weight: Font.Medium; padding: 18 }
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
            Label { text: qsTr("NeverD Agent keeps working on one goal until the goal is completed, paused or reaches a limit."); color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
            ScrollView {
                Layout.fillWidth: true; Layout.preferredHeight: 135; clip: true
                TextArea {
                    id: objective
                    enabled: !editor.backend.running
                    placeholderText: qsTr("Describe the expected outcome and when it is complete…")
                    placeholderTextColor: Theme.quiet; color: Theme.text
                    wrapMode: TextEdit.Wrap; selectByMouse: true; padding: 12
                    background: Rectangle { color: Theme.raised; radius: Theme.controlRadius; border.color: objective.activeFocus ? Theme.focusBorder : Theme.inputBorder }
                }
            }
            GridLayout {
                columns: 2; Layout.fillWidth: true; columnSpacing: 16
                Label { text: qsTr("Total token budget"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("Maximum rounds"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: tokens; enabled: !editor.backend.running; validator: IntValidator { bottom: 1 } Layout.fillWidth: true }
                Field { id: rounds; enabled: !editor.backend.running; validator: IntValidator { bottom: 1 } Layout.fillWidth: true }
            }
            Label { text: qsTr("The budget covers input, output, subtasks and context compaction. Each request reserves an estimate first; the server's metering is authoritative. Resuming a goal keeps the budget already used."); color: Theme.quiet; font.pointSize: Theme.smallPointSize; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { visible: !!editor.backend.goal.summary; text: qsTr("Current progress"); color: Theme.text; font.weight: Font.Medium }
            MarkdownView { visible: !!editor.backend.goal.summary; source: editor.backend.goal.summary || ""; Layout.fillWidth: true; onLinkRequested: link => editor.backend.openTextLink(link) }
            Label { visible: !!editor.backend.goal.evidence; text: qsTr("Recorded evidence"); color: Theme.text; font.weight: Font.Medium }
            MarkdownView { visible: !!editor.backend.goal.evidence; source: editor.backend.goal.evidence || ""; Layout.fillWidth: true; onLinkRequested: link => editor.backend.openTextLink(link) }
            Label { visible: !!editor.backend.goal.nextStep; text: qsTr("Next step: %1").arg(editor.backend.goal.nextStep); color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { visible: !!editor.backend.goal.error; text: editor.backend.goal.error || ""; color: Theme.error; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
        }
    }
    footer: RowLayout {
        spacing: 8
        ActionButton {
            Layout.leftMargin: 18; Layout.bottomMargin: 18
            visible: editor.backend.goal.status === "active"
            text: qsTr("Pause goal"); onClicked: editor.backend.pauseGoal()
        }
        Item { Layout.fillWidth: true }
        ActionButton { text: qsTr("Close"); Layout.bottomMargin: 18; onClicked: editor.close() }
        ActionButton {
            text: editor.editing ? qsTr("Save and resume") : qsTr("Start goal")
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
