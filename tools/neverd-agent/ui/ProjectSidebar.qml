import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

// The side list: the application's name on the strip that moves the window,
// the two ways to begin, the projects with their analysis objects, and the
// settings at its foot.
Surface {
    id: sidebar
    color: Theme.surface
    radius: Theme.workspaceRadius
    required property var backend
    property bool windowActive: true
    signal createRequested()
    signal openRequested()
    signal settingsRequested()
    signal menuRequested(string projectId, string objectId, var origin)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.topBarHeight
            WindowDrag { anchors.fill: parent }
            RowLayout {
                anchors.left: parent.left
                anchors.leftMargin: Theme.spaceMedium
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spaceSmall
                Image {
                    source: "qrc:/agent/icons/brand.svg"
                    sourceSize: Qt.size(20, 20)
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                }
                Label {
                    text: "NeverD Agent"
                    color: sidebar.windowActive ? Theme.titleText : Theme.titleInactiveText
                    font.weight: Font.Medium
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceSmall
            Layout.rightMargin: Theme.spaceSmall
            Layout.topMargin: Theme.spaceTiny
            spacing: 2
            ActionButton {
                glyph: "folder_new"
                text: qsTr("New project")
                Layout.fillWidth: true
                onClicked: sidebar.createRequested()
            }
            ActionButton {
                glyph: "open"
                text: qsTr("Open project…")
                Layout.fillWidth: true
                onClicked: sidebar.openRequested()
            }
        }
        Label {
            text: qsTr("Projects")
            color: Theme.secondary
            font.pointSize: Theme.smallPointSize
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceMedium
            Layout.rightMargin: Theme.spaceMedium
            Layout.topMargin: Theme.spaceLarge - Theme.spaceTiny
            Layout.bottomMargin: 6
            elide: Text.ElideRight
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.spaceSmall
            Layout.rightMargin: Theme.spaceSmall
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: 2
                Repeater {
                    model: sidebar.backend.projects
                    delegate: ColumnLayout {
                        id: projectNode
                        required property var modelData
                        property bool expanded: true
                        readonly property bool waiting: sidebar.backend.analysisObjectApprovals.some(
                            request => request.projectId === projectNode.modelData.id)
                        Layout.fillWidth: true
                        spacing: 2
                        SidebarRow {
                            Layout.fillWidth: true
                            text: projectNode.modelData.name
                            glyph: "folder"
                            hint: projectNode.modelData.name + "\n" + projectNode.modelData.directory
                                  + (projectNode.waiting ? "\n" + qsTr("Approval request pending") : "")
                            foldable: true
                            expanded: projectNode.expanded
                            foldHint: projectNode.expanded ? qsTr("Collapse project") : qsTr("Expand project")
                            menuHint: qsTr("Project menu")
                            marked: projectNode.waiting
                            // One row carries the fill: the open project's
                            // until one of its objects is chosen, whose row
                            // then does. The open project's name stays heavier.
                            readonly property bool open: sidebar.backend.activeId === projectNode.modelData.id
                            selected: open && !sidebar.backend.selectedObjectId
                            weight: open ? Font.Medium : Font.Normal
                            onClicked: sidebar.backend.selectProject(projectNode.modelData.id)
                            onFoldRequested: projectNode.expanded = !projectNode.expanded
                            onMenuRequested: origin => sidebar.menuRequested(projectNode.modelData.id, "", origin)
                        }
                        Repeater {
                            model: projectNode.expanded ? projectNode.modelData.objects : []
                            delegate: SidebarRow {
                                required property var modelData
                                Layout.fillWidth: true
                                indent: 20
                                glyph: "file"
                                text: modelData.name
                                elideMode: Text.ElideMiddle
                                hint: modelData.path
                                menuHint: qsTr("Analysis object menu")
                                selected: sidebar.backend.activeId === projectNode.modelData.id
                                          && sidebar.backend.selectedObjectId === modelData.id
                                onClicked: {
                                    if (sidebar.backend.selectProject(projectNode.modelData.id))
                                        sidebar.backend.selectObject(modelData.id)
                                }
                                onMenuRequested: origin => sidebar.menuRequested(projectNode.modelData.id, modelData.id, origin)
                            }
                        }
                        Label {
                            visible: projectNode.expanded && projectNode.modelData.objects.length === 0
                            text: qsTr("No analysis objects yet")
                            color: Theme.quiet
                            font.pointSize: Theme.smallPointSize
                            Layout.fillWidth: true
                            Layout.leftMargin: 28
                            Layout.topMargin: 4
                            Layout.bottomMargin: 6
                            elide: Text.ElideRight
                        }
                    }
                }
                Label {
                    visible: sidebar.backend.projects.length === 0
                    text: qsTr("Create a project for one line of research, then add the files to analyze together.")
                    color: Theme.quiet
                    font.pointSize: Theme.smallPointSize
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spaceSmall
                    Layout.rightMargin: Theme.spaceSmall
                }
            }
        }
        ActionButton {
            text: qsTr("Workspace settings…")
            glyph: "options"
            Layout.fillWidth: true
            Layout.margins: Theme.spaceSmall
            onClicked: sidebar.settingsRequested()
        }
    }
}
