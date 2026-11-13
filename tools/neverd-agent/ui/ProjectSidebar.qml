import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Surface {
    id: sidebar
    color: Theme.surface
    border.width: 1
    border.color: Theme.border
    radius: Theme.workspaceRadius
    required property var backend
    signal createRequested()
    signal openRequested()
    signal settingsRequested()
    signal menuRequested(string projectId, string objectId, var origin)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceMedium
        spacing: Theme.spaceMedium
        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTr("项目"); color: Theme.secondary; font.pointSize: Theme.smallPointSize; font.weight: Font.Medium; Layout.fillWidth: true }
            ActionButton { glyph: "open"; hint: qsTr("打开项目"); onClicked: sidebar.openRequested() }
            ActionButton { glyph: "folder_new"; hint: qsTr("新建项目"); onClicked: sidebar.createRequested() }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: Theme.spaceMedium
                Repeater {
                    model: sidebar.backend.projects
                    delegate: ColumnLayout {
                        id: projectNode
                        required property var modelData
                        property bool expanded: true
                        Layout.fillWidth: true
                        spacing: 4
                        RowLayout {
                            HoverHandler { id: projectHover }
                            Layout.fillWidth: true
                            spacing: 0
                            ActionButton {
                                glyph: projectNode.expanded ? "chevron_down" : "chevron_right"
                                hint: projectNode.expanded ? qsTr("折叠项目") : qsTr("展开项目")
                                onClicked: projectNode.expanded = !projectNode.expanded
                                Layout.preferredWidth: 26
                            }
                            ActionButton {
                                id: projectButton
                                text: projectNode.modelData.name
                                glyph: "folder"
                                hint: projectNode.modelData.name + "\n" + projectNode.modelData.directory
                                      + (approvalDot.visible ? "\n" + qsTr("有待审批请求") : "")
                                checked: sidebar.backend.activeId === projectNode.modelData.id
                                font.weight: checked ? Font.Medium : Font.Normal
                                Layout.fillWidth: true
                                onClicked: sidebar.backend.selectProject(projectNode.modelData.id)
                                Keys.onMenuPressed: sidebar.menuRequested(projectNode.modelData.id, "", projectButton)
                                Rectangle {
                                    visible: projectButton.checked
                                    width: 2; height: parent.height - 14
                                    x: 1; anchors.verticalCenter: parent.verticalCenter
                                    radius: 1; color: Theme.accent
                                }
                                Rectangle {
                                    id: approvalDot
                                    visible: sidebar.backend.analysisObjectApprovals.some(request =>
                                        request.projectId === projectNode.modelData.id)
                                    width: 6; height: 6; radius: 3
                                    anchors.right: parent.right; anchors.rightMargin: 4
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: Theme.accent
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.RightButton
                                    onClicked: sidebar.menuRequested(projectNode.modelData.id, "", projectButton)
                                }
                            }
                            ActionButton {
                                glyph: "more"
                                hint: qsTr("项目菜单")
                                opacity: projectHover.hovered || activeFocus || hovered ? 1 : 0
                                onClicked: sidebar.menuRequested(projectNode.modelData.id, "", this)
                                Layout.preferredWidth: 28
                            }
                        }
                        ColumnLayout {
                            visible: projectNode.expanded
                            Layout.fillWidth: true
                            Layout.leftMargin: 24
                            spacing: 3
                            Repeater {
                                model: projectNode.modelData.objects
                                delegate: RowLayout {
                                    id: objectNode
                                    HoverHandler { id: objectHover }
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 0
                                    ActionButton {
                                        id: objectButton
                                        glyph: "file"
                                        text: objectNode.modelData.name
                                        elideMode: Text.ElideMiddle
                                        hint: objectNode.modelData.path
                                        Layout.fillWidth: true
                                        checked: sidebar.backend.activeId === projectNode.modelData.id
                                                 && sidebar.backend.selectedObjectId === objectNode.modelData.id
                                        onClicked: {
                                            if (sidebar.backend.selectProject(projectNode.modelData.id))
                                                sidebar.backend.selectObject(objectNode.modelData.id)
                                        }
                                        Keys.onMenuPressed: sidebar.menuRequested(projectNode.modelData.id, objectNode.modelData.id, objectButton)
                                        Rectangle {
                                            visible: objectButton.checked
                                            width: 2; height: parent.height - 14
                                            x: 1; anchors.verticalCenter: parent.verticalCenter
                                            radius: 1; color: Theme.accent
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            acceptedButtons: Qt.RightButton
                                            onClicked: sidebar.menuRequested(projectNode.modelData.id, objectNode.modelData.id, objectButton)
                                        }
                                    }
                                    ActionButton {
                                        glyph: "more"
                                        hint: qsTr("分析对象菜单")
                                        opacity: objectHover.hovered || activeFocus || hovered ? 1 : 0
                                        onClicked: sidebar.menuRequested(projectNode.modelData.id, objectNode.modelData.id, this)
                                        Layout.preferredWidth: 28
                                    }
                                }
                            }
                            Label {
                                visible: projectNode.modelData.objects.length === 0
                                text: qsTr("还没有分析对象")
                                color: Theme.secondary
                                Layout.margins: 8
                            }
                        }
                    }
                }
                Label {
                    visible: sidebar.backend.projects.length === 0
                    text: qsTr("为一项研究建立项目，\n再添加需要一起分析的文件。")
                    color: Theme.secondary
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.topMargin: 16
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
        ActionButton {
            text: qsTr("工作区设置")
            glyph: "options"
            Layout.fillWidth: true
            onClicked: sidebar.settingsRequested()
        }
    }
}
