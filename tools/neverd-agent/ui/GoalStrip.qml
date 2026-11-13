import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Surface {
    id: strip
    required property var backend
    signal editRequested()
    color: Theme.surface; radius: 9; border.width: 1
    implicitHeight: body.implicitHeight + 20
    readonly property var goal: backend.goal
    readonly property string stateText: ({active: qsTr("推进中"), paused: qsTr("已暂停"), completed: qsTr("已完成"), blocked: qsTr("等待处理"), budget_exhausted: qsTr("预算已用完"), round_limit: qsTr("达到轮次上限"), interrupted: qsTr("上次运行已中断"), failed: qsTr("运行失败")}[goal.status] || qsTr("目标"))
    ColumnLayout {
        id: body
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.margins: 10; spacing: 5
        RowLayout {
            Layout.fillWidth: true
            Icon { name: "bookmark"; Layout.preferredWidth: Theme.iconSize; Layout.preferredHeight: Theme.iconSize }
            Label { text: strip.goal.objective || ""; color: Theme.text; elide: Text.ElideRight; Layout.fillWidth: true }
            ActionButton {
                text: strip.goal.status === "active" ? qsTr("暂停") : qsTr("继续")
                visible: strip.goal.status !== "completed"
                enabled: strip.goal.status === "active" || !strip.backend.running
                implicitHeight: 26
                onClicked: {
                    if (strip.goal.status === "active") strip.backend.pauseGoal()
                    else if (strip.goal.usedTokens >= strip.goal.tokenBudget || strip.goal.rounds >= strip.goal.roundLimit) strip.editRequested()
                    else strip.backend.resumeGoal()
                }
            }
            ActionButton { text: qsTr("详情"); implicitHeight: 26; onClicked: strip.editRequested() }
        }
        Label {
            text: strip.stateText + qsTr("  ·  第 %1 / %2 轮  ·  %3%4 / %5 tokens").arg(strip.goal.rounds || 0).arg(strip.goal.roundLimit || 0).arg(strip.goal.estimated ? "≈ " : "").arg(Number(strip.goal.usedTokens || 0).toLocaleString(Qt.locale(), 'f', 0)).arg(Number(strip.goal.tokenBudget || 0).toLocaleString(Qt.locale(), 'f', 0))
            color: Theme.quiet; font.pointSize: Theme.smallPointSize
            elide: Text.ElideRight; Layout.fillWidth: true
        }
    }
}
