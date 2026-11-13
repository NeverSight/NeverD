import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Surface {
    id: card
    objectName: "analysisObjectApprovalCard"
    required property var request
    signal decision(string requestId, bool allow)
    readonly property bool narrow: width < 290
    implicitHeight: content.implicitHeight + 24
    implicitWidth: 300
    color: Theme.composerTint
    border.width: 1
    border.color: Theme.borderStrong
    radius: 16
    Accessible.name: title.text

    function choose(allow) {
        if (request && request.id)
            decision(request.id, allow)
    }
    Keys.onEscapePressed: event => { choose(false); event.accepted = true }

    ColumnLayout {
        id: content
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        anchors.margins: 12
        spacing: Theme.spaceSmall

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Icon { name: "file_load"; color: Theme.secondary; Layout.preferredWidth: 16; Layout.preferredHeight: 16 }
            Label {
                id: title
                text: qsTr("允许添加分析对象？")
                color: Theme.text; font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(pathPreview.implicitHeight, 76)
            contentWidth: availableWidth
            clip: true
            background: Rectangle { radius: Theme.controlRadius; color: Theme.code }
            ScrollBar.vertical: ThemedScrollBar {}
            TextEdit {
                id: pathPreview
                width: parent.width
                padding: 6; leftPadding: 8; rightPadding: 8
                text: card.request ? card.request.path : ""
                textFormat: TextEdit.PlainText; readOnly: true; selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                color: Theme.text; font.family: "JetBrains Mono NL"; font.pointSize: Theme.smallPointSize
                Accessible.name: qsTr("要添加的文件路径")
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(details.implicitHeight, 80)
            contentWidth: availableWidth; contentHeight: details.implicitHeight
            clip: true
            ScrollBar.vertical: ThemedScrollBar {}
            ColumnLayout {
                id: details
                width: parent.width
                spacing: 3
                Label {
                    Layout.fillWidth: true
                    text: card.request ? (card.request.source
                        ? qsTr("%1 · 来自 %2").arg(card.request.project).arg(card.request.source)
                        : card.request.project) : ""
                    textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere
                    color: Theme.secondary; font.pointSize: Theme.smallPointSize
                    ThemedToolTip { visible: projectHover.hovered; text: card.request ? card.request.projectPath : "" }
                    HoverHandler { id: projectHover }
                }
                Label {
                    Layout.fillWidth: true
                    text: card.request ? card.request.reason : ""
                    textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere
                    color: Theme.secondary; font.pointSize: Theme.smallPointSize
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: card.narrow ? 1 : 3
            columnSpacing: Theme.spaceSmall; rowSpacing: 4
            Item { visible: !card.narrow; Layout.fillWidth: true }
            ApprovalButton {
                text: qsTr("拒绝")
                objectName: "denyAnalysisObject"
                allow: false
                Layout.fillWidth: card.narrow
                onDecision: allow => card.choose(allow)
            }
            ApprovalButton {
                text: qsTr("允许一次")
                objectName: "approveAnalysisObject"
                allow: true
                Layout.fillWidth: card.narrow
                onDecision: allow => card.choose(allow)
            }
        }
    }

    component ApprovalButton: ActionButton {
        required property bool allow
        signal decision(bool allow)
        primary: allow
        onClicked: decision(allow)
        Keys.onReturnPressed: event => { decision(allow); event.accepted = true }
        Keys.onEnterPressed: event => { decision(allow); event.accepted = true }
    }
}
