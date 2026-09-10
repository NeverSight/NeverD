pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property var controller
    property string selectedId: ""
    property string selectedNamespace: ""
    property var kddockwidgets_min_size: Qt.size(500, 160)
    signal importRequested()
    color: Theme.editor
    RowLayout {
        anchors.fill: parent
        spacing: 0
        ColumnLayout {
            Layout.preferredWidth: 290
            Layout.fillHeight: true
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 6
                spacing: 3
                WorkbenchButton { text: qsTr("Import Manifest…"); onClicked: root.importRequested() }
                WorkbenchButton { text: qsTr("Run"); enabled: root.selectedId.length > 0; onClicked: root.controller.runContribution(root.selectedId) }
                WorkbenchButton { text: qsTr("Unload"); enabled: root.selectedNamespace.length > 0; onClicked: { root.controller.unloadContributions(root.selectedNamespace); root.selectedId = ""; root.selectedNamespace = "" } }
            }
            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: root.controller.contributions
                clip: true
                reuseItems: true
                ScrollBar.vertical: WorkbenchScrollBar { active: true }
                delegate: ItemDelegate {
                    id: entry
                    required property var modelData
                    width: list.width
                    height: 48
                    hoverEnabled: true
                    leftPadding: 12
                    rightPadding: 12
                    contentItem: ColumnLayout {
                        spacing: 3
                        Text { textFormat: Text.PlainText; text: entry.modelData.title; color: Theme.foreground; font.pointSize: Theme.bodySize; Layout.fillWidth: true; elide: Text.ElideRight }
                        Text { textFormat: Text.PlainText; text: entry.modelData.namespace + " · " + entry.modelData.kind; color: Theme.subdued; font.pointSize: Theme.captionSize; Layout.fillWidth: true; elide: Text.ElideRight }
                    }
                    background: Rectangle { color: root.selectedId === entry.modelData.id ? Theme.selection : entry.hovered ? Theme.hover : "transparent"; border.color: entry.activeFocus ? Theme.focus : "transparent" }
                    onClicked: { root.selectedId = modelData.id; root.selectedNamespace = modelData.namespace }
                    onDoubleClicked: root.controller.runContribution(modelData.id)
                    Accessible.name: modelData.title
                }
                EmptyPane { anchors.fill: parent; visible: list.count === 0; title: ""; detail: qsTr("Import a declarative manifest to add analysis commands and views.") }
            }
        }
        Rectangle { Layout.fillHeight: true; implicitWidth: 1; color: Theme.border }
        TextPane { Layout.fillWidth: true; Layout.fillHeight: true; text: root.controller.contributionResult; emptyTitle: ""; emptyDetail: qsTr("Select an extension command to inspect its result."); codePointSize: Theme.bodySize }
    }
}
