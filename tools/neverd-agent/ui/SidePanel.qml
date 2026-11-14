import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Surface {
    id: panel
    // A pane beside the conversation; a card when it floats over a narrow window.
    property bool floating: false
    color: Theme.surface
    border.width: floating ? 1 : 0
    border.color: Theme.borderStrong
    radius: floating ? Theme.radius : Theme.workspaceRadius
    required property var backend
    property var views: []
    property int currentView: 0
    readonly property var activeView: views[currentView] || {kind: "files", objectId: ""}
    signal closeRequested()
    signal newViewRequested(var origin)
    signal addMemoryRequested(string objectId)
    signal editMemoryRequested(string objectId, string id, string text)
    signal transferRequested(string objectId, bool importing)
    signal contextRequested()
    signal viewSelected(int index)
    onCurrentViewChanged: {
        if (viewTabs.currentIndex !== currentView)
            viewTabs.currentIndex = currentView
        const view = views[currentView]
        if (view && view.objectId)
            backend.selectObject(view.objectId)
        if (view && view.kind === "evidence" && view.evidenceId && backend.evidence.id !== view.evidenceId)
            backend.openEvidence(view.evidenceId)
    }

    function viewTitle(view) {
        switch (view.kind) {
        case "review": return qsTr("Change review")
        case "research": return qsTr("Research log")
        case "evidence": return view.title || qsTr("Function evidence")
        case "chat": return qsTr("Side chat")
        case "files": return qsTr("Project files")
        case "progress": return qsTr("%1 · Progress").arg(view.objectName || "")
        default: return qsTr("%1 · Memory").arg(view.objectName || "")
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: Theme.spaceMedium
        RowLayout {
            Layout.fillWidth: true
            TabBar {
                id: viewTabs
                Layout.fillWidth: true
                // The window owns the current view; a tab click asks for it,
                // so a view the window opens later still shows.
                currentIndex: panel.currentView
                onCurrentIndexChanged: if (currentIndex !== panel.currentView) panel.viewSelected(currentIndex)
                background: null
                Repeater {
                    model: panel.views
                    delegate: TabButton {
                        required property var modelData
                        text: modelData.kind === "review" ? qsTr("Changes") : modelData.kind === "research" ? qsTr("Research") : modelData.kind === "evidence" ? qsTr("Evidence") : modelData.kind === "chat" ? qsTr("Chat") : modelData.kind === "files" ? qsTr("Files") : modelData.kind === "progress" ? qsTr("Progress") : qsTr("Memory")
                        id: tab
                        // As wide as its name, so no language cuts it off;
                        // the bar scrolls when the names outgrow it.
                        width: implicitWidth
                        implicitWidth: Math.max(56, contentItem.implicitWidth + 20)
                        implicitHeight: 30
                        font.pointSize: Theme.smallPointSize
                        contentItem: Text {
                            text: parent.text
                            color: tab.checked ? Theme.text : Theme.secondary
                            font: parent.font
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        // Selected tab: accent underline; hover: soft fill. No boxed outline.
                        background: Item {
                            Rectangle {
                                anchors.fill: parent; anchors.bottomMargin: 3
                                radius: Theme.toolRadius
                                color: tab.hovered && !tab.checked ? Theme.hover : "transparent"
                            }
                            Rectangle {
                                visible: tab.checked
                                anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom
                                width: parent.width - 20; height: 2; radius: 1
                                color: Theme.accent
                            }
                        }
                        ThemedToolTip { visible: tab.hovered; text: panel.viewTitle(modelData) }
                    }
                }
            }
            ActionButton { glyph: "plus"; hint: qsTr("Open sidebar view"); onClicked: panel.newViewRequested(this) }
            ActionButton { glyph: "close_small"; hint: qsTr("Hide sidebar"); onClicked: panel.closeRequested() }
        }
        Label {
            text: panel.backend.activeProject.name || qsTr("Open a project first")
            color: Theme.secondary
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        EvidenceView {
            visible: panel.activeView.kind === "evidence"
            Layout.fillWidth: true; Layout.fillHeight: true
            backend: panel.backend
        }
        ReviewView { visible: panel.activeView.kind === "review"; Layout.fillWidth: true; Layout.fillHeight: true; backend: panel.backend }
        ResearchView { visible: panel.activeView.kind === "research"; Layout.fillWidth: true; Layout.fillHeight: true; backend: panel.backend }
        ColumnLayout {
            visible: panel.activeView.kind === "files"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8
            Label {
                text: panel.backend.activeProject.directory || ""
                textFormat: Text.PlainText
                color: Theme.secondary
                font.pointSize: Theme.smallPointSize
                elide: Text.ElideMiddle
                Layout.fillWidth: true
                HoverHandler { id: pathHover }
                ThemedToolTip { visible: pathHover.hovered; text: parent.text }
            }
            ListView {
                id: fileList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: panel.backend.files
                ScrollBar.vertical: ThemedScrollBar {}
                delegate: ActionButton {
                    required property var modelData
                    width: fileList.width
                    text: (modelData.directory ? (modelData.expanded ? "⌄ " : "› ") : "") + modelData.name
                    glyph: modelData.directory ? "folder" : "file"
                    elideMode: Text.ElideMiddle
                    hint: modelData.path
                    leftPadding: 8 + modelData.depth * 14
                    onClicked: modelData.directory ? panel.backend.toggleDirectory(modelData.path) : panel.backend.previewFile(modelData.path)
                }
                Label {
                    visible: fileList.count === 0
                    width: parent.width
                    y: 24
                    text: qsTr("The project folder has no files yet.\nNotes, scripts and reports appear here.")
                    color: Theme.secondary
                    wrapMode: Text.Wrap
                }
            }
            RowLayout {
                ActionButton { glyph: "open"; text: qsTr("Open folder"); onClicked: panel.backend.showProjectFolder() }
                Item { Layout.fillWidth: true }
                ActionButton { glyph: "refresh"; hint: qsTr("Refresh files"); onClicked: panel.backend.refreshFiles() }
            }
        }
        ColumnLayout {
            visible: panel.activeView.kind === "progress" || panel.activeView.kind === "memory"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12
            ChoiceField {
                id: objectPicker
                Layout.fillWidth: true
                textRole: "name"
                valueRole: "id"
                model: panel.backend.activeProject.objects || []
                palette.text: Theme.text
                palette.buttonText: Theme.text
                palette.button: Theme.raised
                palette.base: Theme.raised
                onActivated: panel.backend.selectObject(currentValue)
                Accessible.name: qsTr("Analysis objects in the current project")
            }
            Label {
                visible: objectPicker.count === 0
                text: qsTr("Add an analysis object to the project first.")
                color: Theme.secondary
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            ScrollView {
                visible: panel.activeView.kind === "progress" && objectPicker.count > 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    width: parent.width
                    spacing: 16
                    Label {
                        text: panel.backend.running ? qsTr("Analyzing") : qsTr("Analysis progress")
                        color: Theme.text
                        font.pointSize: Theme.bodyPointSize
                    }
                    Label {
                        text: qsTr("Analysis tasks, tool calls and subtask states for the current project appear here.")
                        color: Theme.secondary
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        lineHeight: 1.5
                    }
                    Repeater {
                        model: panel.backend.progress
                        delegate: ColumnLayout {
                            required property var modelData
                            visible: !modelData.objectId || modelData.objectId === panel.backend.selectedObjectId
                            Layout.fillWidth: true
                            Label { text: modelData.label + " · " + ({queued: qsTr("Queued"), running: qsTr("Running"), succeeded: qsTr("Done"), failed: qsTr("Failed"), cancelled: qsTr("Stopped")}[modelData.state] || qsTr("Preparing")); color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            Label { text: modelData.activity; color: Theme.secondary; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                            ContextMeter { usage: modelData.context || ({}) }
                        }
                    }
                    Rectangle { height: 1; color: Theme.border; Layout.fillWidth: true }
                    Label { text: qsTr("Original file"); color: Theme.secondary }
                    Label {
                        text: panel.backend.selectedObject.path || ""
                        textFormat: Text.PlainText
                        color: Theme.text
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                    }
                    Label { text: "SHA-256"; color: Theme.secondary }
                    Label {
                        text: panel.backend.selectedObject.sha256 || ""
                        color: Theme.secondary
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                    }
                    ActionButton {
                        text: qsTr("Open in the NeverD workbench")
                        glyph: "float"
                        onClicked: panel.backend.openInGui(panel.backend.selectedObjectId)
                    }
                }
            }
            MemoryView {
                visible: panel.activeView.kind === "memory"
                Layout.fillWidth: true
                Layout.fillHeight: true
                backend: panel.backend
                onAddRequested: objectId => panel.addMemoryRequested(objectId)
                onEditRequested: (objectId, id, text) => panel.editMemoryRequested(objectId, id, text)
            }
            Item { visible: panel.activeView.kind === "progress" && objectPicker.count === 0; Layout.fillHeight: true }
            Flow {
                visible: panel.activeView.kind === "memory" && objectPicker.count > 0
                Layout.fillWidth: true
                spacing: 4
                ActionButton { text: qsTr("Add…"); glyph: "comment"; onClicked: panel.addMemoryRequested(panel.backend.selectedObjectId) }
                ActionButton { text: qsTr("Import…"); glyph: "file_load"; onClicked: panel.transferRequested(panel.backend.selectedObjectId, true) }
                ActionButton { text: qsTr("Export…"); glyph: "file_produce"; onClicked: panel.transferRequested(panel.backend.selectedObjectId, false) }
                ActionButton { glyph: "refresh"; hint: qsTr("Refresh memory"); onClicked: panel.backend.refreshMemory() }
            }
        }
        SideChatView {
            visible: panel.activeView.kind === "chat"
            Layout.fillWidth: true; Layout.fillHeight: true
            backend: panel.backend
            onContextRequested: panel.contextRequested()
        }
    }
    function syncObject() {
        objectPicker.currentIndex = objectPicker.indexOfValue(panel.backend.selectedObjectId)
    }
    onActiveViewChanged: {
        if (activeView.objectId)
            backend.selectObject(activeView.objectId)
        else if ((activeView.kind === "progress" || activeView.kind === "memory") && !backend.selectedObjectId && objectPicker.count > 0)
            backend.selectObject(objectPicker.valueAt(0))
        Qt.callLater(syncObject)
    }
    Connections {
        target: panel.backend
        function onObjectChanged() { panel.syncObject() }
        function onWorkspaceChanged() { Qt.callLater(panel.syncObject) }
    }
}
