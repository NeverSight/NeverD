import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import NeverD.Agent

ApplicationWindow {
    id: window
    objectName: "agentWindow"
    width: Math.min(1360, Screen.desktopAvailableWidth * 0.92)
    height: Math.min(900, Screen.desktopAvailableHeight * 0.92)
    minimumWidth: 800
    minimumHeight: 560
    visible: false
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint
           | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint | Qt.WindowCloseButtonHint
    title: "NeverD Agent" + (backend.activeProject.name ? " — " + backend.activeProject.name : "")
    color: "transparent"
    font: appFont
    palette.window: Theme.background
    palette.windowText: Theme.text
    palette.text: Theme.text
    palette.base: Theme.raised
    palette.button: Theme.raised
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.background
    // Roles Basic-style controls draw borders, tooltips and the modal dim from.
    palette.dark: Theme.borderStrong
    palette.mid: Theme.border
    palette.midlight: Theme.selected
    palette.light: Theme.hover
    palette.shadow: Theme.shadow
    palette.toolTipBase: Theme.menu
    palette.toolTipText: Theme.text
    palette.placeholderText: Theme.quiet
    palette.link: Theme.accent

    property bool sideOpen: false
    property string menuObject: ""
    property string lastProject: ""
    property string noticeText: ""
    property var sideViews: []
    property int sideIndex: 0
    property string memoryObject: ""
    property string memoryRecordId: ""

    function popupAt(menu, origin) {
        const point = origin.mapToItem(Overlay.overlay, 0, origin.height)
        menu.x = Math.min(point.x, window.width - menu.width - 12)
        menu.y = point.y
        menu.open()
    }
    function openSide(kind, objectId) {
        if (!backend.activeId) return
        if (kind === "chat" && !backend.sideChat && !backend.createSideChat()) return
        if (objectId) backend.selectObject(objectId)
        const key = kind + ":" + (objectId || "")
        const found = sideViews.findIndex(v => v.key === key)
        if (found >= 0) sideIndex = found
        else {
            sideViews = sideViews.concat([{key: key, kind: kind, objectId: objectId || "",
                title: kind === "review" ? qsTr("变更审阅") : kind === "research" ? qsTr("研究记录") : kind === "evidence" ? qsTr("函数证据") : kind === "chat" ? qsTr("侧边聊天") : kind === "files" ? qsTr("项目文件") : (backend.selectedObject.name || "") + (kind === "progress" ? qsTr(" · 进度") : qsTr(" · 记忆"))}])
            sideIndex = sideViews.length - 1
        }
        sideOpen = true
        if (kind === "files") backend.refreshFiles()
    }
    function showSettings() {
        sharingCheck.checked = backend.sharing
        useMemoryCheck.checked = backend.useMemory
        generateMemoryCheck.checked = backend.generateMemory
        themePicker.currentIndex = backend.lightTheme ? 1 : 0
        objectApprovalMode.currentIndex = backend.autoAddAnalysisObjects ? 1 : 0
        guiField.text = backend.guiPath
        settingsDialog.open()
    }
    function showModelSettings() { modelSettingsDialog.open() }
    function showContextPicker(side) { contextPicker.side = side; contextPicker.open() }
    function showEvidence(id, title) {
        const key = "evidence:" + id
        const found = sideViews.findIndex(v => v.key === key)
        if (found >= 0) sideIndex = found
        else {
            const empty = sideViews.findIndex(v => v.kind === "evidence" && !v.evidenceId)
            const entry = {key:key, kind:"evidence", evidenceId:id, objectId:backend.evidence.objectId, title:title}
            if (empty >= 0) { const updated = sideViews.slice(); updated[empty] = entry; sideViews = updated; sideIndex = empty }
            else { sideViews = sideViews.concat([entry]); sideIndex = sideViews.length - 1 }
        }
        sideOpen = true
    }
    function showContext(projectId, objectId, origin) {
        const point = origin.mapToItem(Overlay.overlay, 0, origin.height)
        if (!backend.selectProject(projectId)) return
        menuObject = objectId
        if (objectId) backend.selectObject(objectId)
        contextMenu.x = Math.min(point.x, window.width - contextMenu.width - 12)
        contextMenu.y = point.y
        contextMenu.open()
    }

    Component.onCompleted: {
        Theme.backdrop = workspaceBackdrop
        Theme.basePointSize = appFont.pointSize > 0 ? appFont.pointSize : 10
        Theme.light = backend.lightTheme
        lastProject = backend.activeId
        if (startupError) {
            failureText.text = startupError
            failureDialog.open()
        }
        if (initialSidePanel) openSide(initialSidePanel, "")
    }
    onClosing: close => { close.accepted = backend.prepareClose() }

    Shortcut { sequence: "Ctrl+N"; onActivated: createDialog.open() }
    Shortcut { sequence: "Ctrl+O"; onActivated: openProjectDialog.open() }
    Shortcut { sequence: "Ctrl+,"; onActivated: window.showSettings() }
    Shortcut { sequence: "Ctrl+B"; onActivated: window.sideOpen = !window.sideOpen }

    Item {
        id: shell
        anchors.fill: parent
        anchors.margins: windowFrame.shadowMargin
        Repeater {
            model: windowFrame.shadowMargin
            Rectangle {
                required property int index
                anchors.fill: parent
                anchors.margins: -(index + 1)
                radius: 8 + index
                color: "transparent"
                border.width: 1
                border.color: Qt.rgba(0.02, 0.04, 0.09, 0.2 * Math.pow(1 - index / Math.max(1, windowFrame.shadowMargin), 2))
            }
        }
    }
    Backdrop {
        id: workspaceBackdrop
        anchors.fill: shell
        cornerRadius: windowFrame.maximized ? 0 : 8
    }
    ColumnLayout {
        anchors.fill: shell
        spacing: 0
        enabled: !startupError
        Item {
            id: titleBar
            Layout.fillWidth: true
            Layout.preferredHeight: windowFrame.titleHeight
            MouseArea {
                anchors.fill: parent
                property point pressAt
                property bool moving: false
                onPressed: mouse => { pressAt = Qt.point(mouse.x, mouse.y); moving = false }
                onPositionChanged: mouse => {
                    if (pressed && !moving && Math.abs(mouse.x - pressAt.x) + Math.abs(mouse.y - pressAt.y) >= windowFrame.dragDistance) {
                        moving = true
                        windowFrame.startMove()
                    }
                }
                onDoubleClicked: windowFrame.toggleMaximized()
            }
            RowLayout {
                anchors.left: parent.left
                anchors.leftMargin: 13
                anchors.verticalCenter: parent.verticalCenter
                spacing: 9
                Image { source: "qrc:/agent/icons/brand.svg"; sourceSize: Qt.size(20, 20); Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
                Label { text: "NeverD Agent"; color: window.active ? Theme.titleText : Theme.titleInactiveText; font.pointSize: Theme.smallPointSize }
            }
            Label {
                anchors.centerIn: parent
                width: Math.min(parent.width * 0.35, 420)
                horizontalAlignment: Text.AlignHCenter
                text: backend.activeProject.name || qsTr("研究工作区")
                color: window.active ? Theme.titleText : Theme.titleInactiveText
                font.pointSize: Theme.smallPointSize
                elide: Text.ElideRight
            }
            RowLayout {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                spacing: 0
                ActionButton { glyph: "options"; hint: qsTr("设置"); onClicked: window.showSettings(); implicitHeight: 28 }
                ActionButton {
                    glyph: "comment"
                    readonly property bool waiting: backend.analysisObjectApprovals.some(request =>
                        request.projectId === backend.activeId && !!request.sideChatId)
                    hint: waiting ? qsTr("侧边聊天 · 有待审批请求") : qsTr("侧边聊天")
                    enabled: backend.activeId.length > 0; implicitHeight: 28
                    onClicked: window.openSide("chat", "")
                    Rectangle {
                        visible: parent.waiting
                        anchors.right: parent.right; anchors.top: parent.top
                        anchors.margins: 4
                        width: 6; height: 6; radius: 3; color: Theme.accent
                    }
                }
                ActionButton {
                    id: panelToggle
                    glyph: "subviews"
                    hint: qsTr("显示或隐藏右侧面板 · %1").arg(backend.shortcutText("Ctrl+B"))
                    checked: window.sideOpen
                    enabled: backend.activeId.length > 0
                    implicitHeight: 28
                    Layout.rightMargin: 12
                    onClicked: {
                        if (window.sideOpen) window.sideOpen = false
                        else if (window.sideViews.length) window.sideOpen = true
                        else window.popupAt(viewMenu, this)
                    }
                }
                WindowControl { kind: "minimize"; frame: windowFrame; appWindow: window }
                WindowControl { kind: "maximize"; frame: windowFrame; appWindow: window }
                WindowControl { kind: "close"; frame: windowFrame; appWindow: window }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            Layout.topMargin: 6
            spacing: 8
            ProjectSidebar {
                Layout.preferredWidth: window.width < 1100 ? 214 : 264
                Layout.fillHeight: true
                backend: controller
                onCreateRequested: createDialog.open()
                onOpenRequested: openProjectDialog.open()
                onSettingsRequested: window.showSettings()
                onMenuRequested: (projectId, objectId, origin) => window.showContext(projectId, objectId, origin)
            }
            Conversation {
                Layout.fillWidth: true
                Layout.fillHeight: true
                backend: controller
                onCreateRequested: createDialog.open()
                onOpenRequested: openProjectDialog.open()
                onAddObjectRequested: objectFileDialog.open()
                onFilesRequested: window.openSide("files", "")
                onModelSettingsRequested: window.showModelSettings()
                onGoalRequested: goalEditor.open()
                onContextRequested: window.showContextPicker(false)
            }
            Item {
                id: sideSlot
                visible: window.sideOpen && window.width >= 1100
                Layout.preferredWidth: window.sideViews[window.sideIndex] && ["evidence", "review", "research"].includes(window.sideViews[window.sideIndex].kind) ? 520 : window.sideViews[window.sideIndex] && window.sideViews[window.sideIndex].kind === "chat" ? 380 : 324
                Layout.fillHeight: true
            }
        }
        Label {
            text: backend.busy ? qsTr("正在读取文件指纹…") : window.noticeText || qsTr("本地工作区")
            color: Theme.secondary
            font.pointSize: Theme.smallPointSize
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            Layout.leftMargin: 18
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            Accessible.name: text
        }
    }
    SidePanel {
        id: details
        visible: window.sideOpen
        parent: window.width >= 1100 ? sideSlot : Overlay.overlay
        width: window.width >= 1100 ? parent.width : Math.min(350, window.width - 32)
        height: window.width >= 1100 ? parent.height : window.height - 84
        x: window.width >= 1100 ? 0 : window.width - width - 10
        y: window.width >= 1100 ? 0 : windowFrame.shadowMargin + windowFrame.titleHeight + 8
        backend: controller
        views: window.sideViews
        onContextRequested: window.showContextPicker(true)
        currentView: window.sideIndex
        onCurrentViewChanged: window.sideIndex = currentView
        onCloseRequested: window.sideOpen = false
        onNewViewRequested: origin => window.popupAt(viewMenu, origin)
        onEditMemoryRequested: (objectId, id, text) => {
            window.memoryObject = objectId
            window.memoryRecordId = id
            memoryText.text = text
            memoryDialog.open()
        }
        onAddMemoryRequested: objectId => {
            window.memoryObject = objectId
            window.memoryRecordId = ""
            memoryText.text = ""
            memoryDialog.open()
        }
        onTransferRequested: (objectId, importing) => {
            memoryFileDialog.projectId = backend.activeId
            memoryFileDialog.objectId = objectId
            memoryFileDialog.importing = importing
            memoryFileDialog.open()
        }
    }

    readonly property var backend: controller
    ThemedMenu {
        id: contextMenu
        width: 230
        ThemedMenuItem { text: qsTr("在 ND GUI 中打开"); visible: window.menuObject.length > 0; onTriggered: backend.openInGui(window.menuObject) }
        ThemedMenuItem {
            text: qsTr("重新定位文件…")
            visible: window.menuObject.length > 0
            enabled: !backend.busy && !backend.reviewBusy
            onTriggered: {
                relocateFileDialog.projectId = backend.activeId
                relocateFileDialog.objectId = window.menuObject
                relocateFileDialog.open()
            }
        }
        ThemedMenuItem { text: qsTr("在侧栏显示分析进度"); visible: window.menuObject.length > 0; onTriggered: window.openSide("progress", window.menuObject) }
        ThemedMenuItem { text: qsTr("查看对象记忆"); visible: window.menuObject.length > 0; onTriggered: window.openSide("memory", window.menuObject) }
        ThemedMenuItem { text: qsTr("在侧栏显示项目文件"); visible: !window.menuObject; onTriggered: window.openSide("files", "") }
        ThemedMenuItem { text: qsTr("打开侧边聊天"); onTriggered: window.openSide("chat", "") }
        ThemedMenuItem { text: qsTr("添加分析对象…"); visible: !window.menuObject; onTriggered: objectFileDialog.open() }
    }
    ThemedMenu {
        id: viewMenu
        width: 200
        ThemedMenuItem { text: qsTr("项目文件"); onTriggered: window.openSide("files", "") }
        ThemedMenuItem { text: qsTr("侧边聊天"); onTriggered: window.openSide("chat", "") }
        ThemedMenuItem { text: qsTr("分析进度"); onTriggered: window.openSide("progress", backend.selectedObjectId) }
        ThemedMenuItem { text: qsTr("研究记录"); onTriggered: window.openSide("research", "") }
        ThemedMenuItem { text: qsTr("函数证据"); onTriggered: window.openSide("evidence", backend.selectedObjectId) }
        ThemedMenuItem { text: qsTr("变更审阅"); onTriggered: window.openSide("review", "") }
        ThemedMenuItem { text: qsTr("对象记忆"); onTriggered: window.openSide("memory", backend.selectedObjectId) }
    }

    FolderDialog {
        id: openProjectDialog
        title: qsTr("选择已有项目的文件夹")
        onAccepted: backend.openProject(selectedFolder.toString())
    }
    FolderDialog {
        id: createFolderDialog
        title: qsTr("选择项目工作目录")
        onAccepted: projectFolder.text = backend.pathFromUrl(selectedFolder.toString())
    }
    FileDialog {
        id: objectFileDialog
        title: qsTr("添加分析对象")
        fileMode: FileDialog.OpenFiles
        onAccepted: { for (const file of selectedFiles) backend.addObject(file.toString()) }
    }
    FileDialog {
        id: relocateFileDialog
        property string projectId
        property string objectId
        title: qsTr("选择原分析对象的文件 · 将校验 SHA-256")
        fileMode: FileDialog.OpenFile
        onAccepted: backend.relocateObject(projectId, objectId, selectedFile.toString())
    }
    FileDialog {
        id: guiFileDialog
        title: qsTr("选择 ND GUI 程序")
        onAccepted: guiField.text = backend.pathFromUrl(selectedFile.toString())
    }
    FileDialog {
        id: memoryFileDialog
        property bool importing: true
        property string projectId
        property string objectId
        title: importing ? qsTr("导入对象记忆") : qsTr("导出对象记忆")
        fileMode: importing ? FileDialog.OpenFile : FileDialog.SaveFile
        nameFilters: [qsTr("对象记忆 (*.ndmemory.json)"), qsTr("JSON 文件 (*.json)")]
        defaultSuffix: "ndmemory.json"
        onAccepted: backend.transferMemory(projectId, objectId, selectedFile.toString(), importing)
    }

    component Sheet: Dialog {
        anchors.centerIn: parent
        parent: Overlay.overlay
        width: Math.min(560, window.width - 48)
        modal: true
        padding: 18
        closePolicy: Popup.CloseOnEscape
        background: Surface { glass: true; radius: Theme.workspaceRadius }
        header: Label {
            text: parent.title
            color: Theme.text
            font.pointSize: Theme.bodyPointSize
            font.weight: Font.Medium
            padding: 18
            bottomPadding: 0
        }
    }
    Sheet {
        id: createDialog
        title: qsTr("新建项目")
        contentItem: ColumnLayout {
            spacing: 12
            Label { text: qsTr("项目名称"); color: Theme.secondary }
            Field { id: projectName; placeholderText: qsTr("例如：Windows API 研究"); Layout.fillWidth: true }
            Label { text: qsTr("项目文件夹"); color: Theme.secondary }
            RowLayout {
                Layout.fillWidth: true
                Field { id: projectFolder; placeholderText: qsTr("用于保存笔记、报告与项目记忆"); Layout.fillWidth: true }
                ActionButton { glyph: "open"; text: qsTr("选择"); onClicked: createFolderDialog.open() }
            }
            Label {
                text: qsTr("会在此目录创建 .nd-agent，保存当前项目的独立上下文。分析对象可以位于其他目录。")
                color: Theme.secondary
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.topMargin: 10
                Item { Layout.fillWidth: true }
                ActionButton { text: qsTr("取消"); onClicked: createDialog.close() }
                ActionButton {
                    text: qsTr("创建项目")
                    primary: true
                    enabled: projectName.text.trim().length > 0 && projectFolder.text.trim().length > 0
                    onClicked: {
                        if (backend.createProject(projectName.text, projectFolder.text)) {
                            createDialog.close()
                            projectName.clear()
                            projectFolder.clear()
                        }
                    }
                }
            }
        }
    }
    Sheet {
        id: settingsDialog
        title: qsTr("应用设置")
        contentItem: ColumnLayout {
            spacing: 14
            ActionButton {
                text: qsTr("模型与分析设置…")
                glyph: "options"
                Layout.fillWidth: true
                onClicked: { settingsDialog.close(); modelSettingsDialog.open() }
            }
            RowLayout {
                Label { text: qsTr("外观"); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
                ChoiceField { id: themePicker; model: [qsTr("黑灰"), qsTr("浅色")]; Layout.preferredWidth: 180 }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("AI 添加分析对象"); color: Theme.secondary; Layout.fillWidth: true }
                ChoiceField {
                    id: objectApprovalMode
                    model: [qsTr("逐次审批"), qsTr("自动允许")]
                    Layout.preferredWidth: 180
                    Accessible.name: qsTr("AI 添加分析对象")
                }
            }
            CheckField {
                id: useMemoryCheck
                text: qsTr("分析时召回已有记忆")

            }
            CheckField {
                id: generateMemoryCheck
                text: qsTr("空闲时自动整理长期记忆")

            }
            Label {
                text: qsTr("整理使用当前模型，会产生额外 API 用量。只处理已完成的本轮交流与证据引用，新分析开始时暂停。关闭召回不删除已有记忆。")
                color: Theme.secondary
                wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize
                Layout.fillWidth: true
            }
            CheckField {
                id: sharingCheck
                text: qsTr("全局共享分析对象记忆")

            }
            Label {
                text: qsTr("按文件指纹读取其他已登记项目中的对象记忆。项目对话保持独立，来源记忆只读。")
                color: Theme.secondary
                wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize
                Layout.fillWidth: true
            }
            Label { text: qsTr("ND GUI 程序路径"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
            RowLayout {
                Layout.fillWidth: true
                Field { id: guiField; placeholderText: qsTr("留空时查找相邻目录或 PATH"); Layout.fillWidth: true }
                ActionButton { glyph: "open"; hint: qsTr("选择程序"); onClicked: guiFileDialog.open() }
            }
            Label { text: qsTr("全局数据目录"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
            Label { text: backend.dataDirectory; color: Theme.secondary; wrapMode: Text.WrapAnywhere; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            RowLayout {
                Layout.topMargin: 8
                Item { Layout.fillWidth: true }
                ActionButton { text: qsTr("取消"); onClicked: settingsDialog.close() }
                ActionButton {
                    text: qsTr("保存设置")
                    primary: true
                    onClicked: {
                        if (backend.updateWorkspaceSettings(sharingCheck.checked, themePicker.currentIndex === 1,
                                                            guiField.text, objectApprovalMode.currentIndex === 1) &&
                            backend.updateMemorySettings(useMemoryCheck.checked, generateMemoryCheck.checked))
                            settingsDialog.close()
                    }
                }
            }
        }
    }
    ModelSettings { id: modelSettingsDialog; backend: controller; parent: Overlay.overlay }
    GoalEditor { id: goalEditor; backend: controller; parent: Overlay.overlay }
    Sheet {
        id: memoryDialog
        title: window.memoryRecordId ? qsTr("编辑并确认记忆") : window.memoryObject ? qsTr("添加对象记忆") : qsTr("添加项目记忆")
        contentItem: ColumnLayout {
            spacing: 12
            Label { text: qsTr("记录已确认的事实及其证据来源。"); color: Theme.secondary; Layout.fillWidth: true }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 220
                TextArea {
                    id: memoryText
                    color: Theme.text
                    placeholderTextColor: Theme.secondary
                    placeholderText: qsTr("例如：关键函数、调用关系、地址与验证方式…")
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    background: Rectangle { color: Theme.raised; radius: 8 }
                }
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                ActionButton { text: qsTr("取消"); onClicked: memoryDialog.close() }
                ActionButton {
                    text: qsTr("保存记忆"); primary: true; enabled: memoryText.text.trim().length > 0
                    onClicked: {
                        const ok = window.memoryRecordId ? backend.editMemory(window.memoryObject, window.memoryRecordId, memoryText.text) : backend.addMemory(window.memoryObject, memoryText.text)
                        if (ok) memoryDialog.close()
                    }
                }
            }
        }
    }
    Sheet {
        id: failureDialog
        title: qsTr("操作未完成")
        closePolicy: startupError ? Popup.NoAutoClose : Popup.CloseOnEscape
        contentItem: ColumnLayout {
            spacing: 16
            Label { id: failureText; color: Theme.text; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            ActionButton {
                text: startupError ? qsTr("退出") : qsTr("知道了")
                primary: true
                Layout.alignment: Qt.AlignRight
                onClicked: startupError ? Qt.quit() : failureDialog.close()
            }
        }
    }
    ContextPicker { id: contextPicker; backend: window.backend }
    Connections {
        target: backend
        function onEvidenceOpened(id, title) { window.showEvidence(id, title) }
        function onSideChatOpened() { window.openSide("chat", "") }
        function onNotice(message) { window.noticeText = message }
        function onOperationError(message) { failureText.text = message; failureDialog.open() }
        function onSettingsChanged() { Theme.light = backend.lightTheme }
        function onWorkspaceChanged() {
            if (window.lastProject !== backend.activeId) {
                window.lastProject = backend.activeId
                window.sideViews = []
                window.sideIndex = 0
                if (window.sideOpen) window.openSide("files", "")
            }
        }
    }
}
