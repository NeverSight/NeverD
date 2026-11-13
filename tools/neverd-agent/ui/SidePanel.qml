import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Surface {
    id: panel
    color: Theme.surface
    border.width: 1
    border.color: Theme.border
    radius: Theme.workspaceRadius
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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: Theme.spaceMedium
        RowLayout {
            Layout.fillWidth: true
            TabBar {
                id: viewTabs
                Layout.fillWidth: true
                currentIndex: panel.currentView
                onCurrentIndexChanged: {
                    panel.currentView = currentIndex
                    const view = panel.views[currentIndex]
                    if (view && view.objectId)
                        panel.backend.selectObject(view.objectId)
                    if (view && view.kind === "evidence" && view.evidenceId && panel.backend.evidence.id !== view.evidenceId)
                        panel.backend.openEvidence(view.evidenceId)
                }
                background: null
                Repeater {
                    model: panel.views
                    delegate: TabButton {
                        required property var modelData
                        text: modelData.kind === "review" ? qsTr("变更") : modelData.kind === "research" ? qsTr("研究") : modelData.kind === "evidence" ? qsTr("证据") : modelData.kind === "chat" ? qsTr("聊天") : modelData.kind === "files" ? qsTr("文件") : modelData.kind === "progress" ? qsTr("进度") : qsTr("记忆")
                        id: tab
                        implicitWidth: 56
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
                                radius: 6
                                color: tab.hovered && !tab.checked ? Theme.hover : "transparent"
                            }
                            Rectangle {
                                visible: tab.checked
                                anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom
                                width: parent.width - 20; height: 2; radius: 1
                                color: Theme.accent
                            }
                        }
                        ThemedToolTip { visible: tab.hovered; text: modelData.title }
                    }
                }
            }
            ActionButton { glyph: "plus"; hint: qsTr("打开侧栏视图"); onClicked: panel.newViewRequested(this) }
            ActionButton { glyph: "close_small"; hint: qsTr("隐藏侧栏"); onClicked: panel.closeRequested() }
        }
        Label {
            text: panel.backend.activeProject.name || qsTr("请先打开项目")
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
                    text: qsTr("项目文件夹还没有文件。\n笔记、脚本和报告会显示在这里。")
                    color: Theme.secondary
                    wrapMode: Text.Wrap
                }
            }
            RowLayout {
                ActionButton { glyph: "open"; text: qsTr("打开文件夹"); onClicked: panel.backend.showProjectFolder() }
                Item { Layout.fillWidth: true }
                ActionButton { glyph: "refresh"; hint: qsTr("刷新文件"); onClicked: panel.backend.refreshFiles() }
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
                Accessible.name: qsTr("当前项目的分析对象")
            }
            Label {
                visible: objectPicker.count === 0
                text: qsTr("先在项目中添加一个分析对象。")
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
                        text: panel.backend.running ? qsTr("正在分析") : qsTr("分析进度")
                        color: Theme.text
                        font.pointSize: Theme.bodyPointSize
                    }
                    Label {
                        text: qsTr("这里显示当前项目的分析任务、工具调用和子任务状态。")
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
                            Label { text: modelData.label + " · " + ({queued: qsTr("等待中"), running: qsTr("运行中"), succeeded: qsTr("完成"), failed: qsTr("失败"), cancelled: qsTr("停止")}[modelData.state] || qsTr("准备中")); color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            Label { text: modelData.activity; color: Theme.secondary; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                            ContextMeter { usage: modelData.context || ({}) }
                        }
                    }
                    Rectangle { height: 1; color: Theme.border; Layout.fillWidth: true }
                    Label { text: qsTr("原始文件"); color: Theme.secondary }
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
                        text: qsTr("在 ND GUI 中打开")
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
                ActionButton { text: qsTr("添加"); glyph: "comment"; onClicked: panel.addMemoryRequested(panel.backend.selectedObjectId) }
                ActionButton { text: qsTr("导入"); glyph: "file_load"; onClicked: panel.transferRequested(panel.backend.selectedObjectId, true) }
                ActionButton { text: qsTr("导出"); glyph: "file_produce"; onClicked: panel.transferRequested(panel.backend.selectedObjectId, false) }
                ActionButton { glyph: "refresh"; hint: qsTr("刷新记忆"); onClicked: panel.backend.refreshMemory() }
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
