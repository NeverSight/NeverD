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
        model: view.backend.sideChat ? [qsTr("Main conversation"), qsTr("Current side chat")] : [qsTr("Main conversation")]
        onActivated: view.refresh()
    }
    ScrollView {
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
        ColumnLayout {
            width: parent.width; spacing: 14
            Label { visible: scope.currentIndex === 0 && !!view.backend.goal.objective; text: qsTr("Persistent goal"); color: Theme.secondary }
            Label { visible: scope.currentIndex === 0; text: view.backend.goal.objective || ""; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Label { text: qsTr("Research steps"); color: Theme.secondary }
            Label { visible: !(view.plan.steps || []).length; text: qsTr("For a complex analysis, you can ask the assistant to list steps and update them as the analysis proceeds. Steps do not create model requests on their own."); color: Theme.quiet; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Repeater {
                model: view.plan.steps || []
                delegate: Surface {
                    id: step; required property var modelData
                    Layout.fillWidth: true; implicitHeight: body.implicitHeight + 24; radius: Theme.radius
                    ColumnLayout {
                        id: body; anchors.fill: parent; anchors.margins: 12; spacing: 7
                        Label { text: ({pending:qsTr("Not started"),in_progress:qsTr("In progress"),completed:qsTr("Completed"),blocked:qsTr("Blocked")}[step.modelData.status] || "") + " · " + step.modelData.title; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        Label { visible: !!step.modelData.summary; text: step.modelData.summary; color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        Repeater { model: step.modelData.evidence_ids; ActionButton { required property string modelData; text: qsTr("Evidence · %1").arg(modelData.slice(0, 8)); onClicked: view.backend.openEvidence(modelData) } }
                        Label { visible: step.modelData.status === "completed" && !step.modelData.evidence_ids.length; text: qsTr("No NeverD evidence is attached to this step."); color: Theme.quiet; font.pointSize: Theme.smallPointSize }
                    }
                }
            }
            Label { text: qsTr("Open questions"); color: Theme.secondary }
            Label { visible: !(view.plan.questions || []).length; text: qsTr("Nothing recorded yet"); color: Theme.quiet }
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
