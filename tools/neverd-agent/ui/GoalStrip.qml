import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Surface {
    id: strip
    required property var backend
    signal editRequested()
    color: Theme.surface; radius: Theme.radius; border.width: 1
    implicitHeight: body.implicitHeight + 20
    readonly property var goal: backend.goal
    readonly property string stateText: ({active: qsTr("In progress"), paused: qsTr("Paused"), completed: qsTr("Completed"), blocked: qsTr("Needs attention"), budget_exhausted: qsTr("Budget used up"), round_limit: qsTr("Round limit reached"), interrupted: qsTr("Last run interrupted"), failed: qsTr("Run failed")}[goal.status] || qsTr("Goal"))
    ColumnLayout {
        id: body
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.margins: 10; spacing: 5
        RowLayout {
            Layout.fillWidth: true
            Icon { name: "bookmark"; Layout.preferredWidth: Theme.iconSize; Layout.preferredHeight: Theme.iconSize }
            Label { text: strip.goal.objective || ""; color: Theme.text; elide: Text.ElideRight; Layout.fillWidth: true }
            ActionButton {
                text: strip.goal.status === "active" ? qsTr("Pause") : qsTr("Resume")
                visible: strip.goal.status !== "completed"
                enabled: strip.goal.status === "active" || !strip.backend.running
                implicitHeight: 26
                onClicked: {
                    if (strip.goal.status === "active") strip.backend.pauseGoal()
                    else if (strip.goal.usedTokens >= strip.goal.tokenBudget || strip.goal.rounds >= strip.goal.roundLimit) strip.editRequested()
                    else strip.backend.resumeGoal()
                }
            }
            ActionButton { text: qsTr("Details…"); implicitHeight: 26; onClicked: strip.editRequested() }
        }
        Label {
            text: qsTr("%1  ·  Round %2 of %3  ·  %4%5 / %6 tokens").arg(strip.stateText).arg(strip.goal.rounds || 0).arg(strip.goal.roundLimit || 0).arg(strip.goal.estimated ? "≈ " : "").arg(Number(strip.goal.usedTokens || 0).toLocaleString(Qt.locale(), 'f', 0)).arg(Number(strip.goal.tokenBudget || 0).toLocaleString(Qt.locale(), 'f', 0))
            color: Theme.quiet; font.pointSize: Theme.smallPointSize
            elide: Text.ElideRight; Layout.fillWidth: true
        }
    }
}
