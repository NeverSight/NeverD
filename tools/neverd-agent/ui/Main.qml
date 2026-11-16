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
    // The workbench's palette roles, as its Theme builds the application's.
    palette.window: Theme.chrome("Window")
    palette.windowText: Theme.chrome("WindowText")
    palette.text: Theme.chrome("Text")
    palette.base: Theme.chrome("Base")
    palette.alternateBase: Theme.chrome("AlternateBase")
    palette.button: Theme.chrome("Button")
    palette.buttonText: Theme.chrome("ButtonText")
    palette.highlight: Theme.chrome("Highlight")
    palette.highlightedText: Theme.chrome("HighlightedText")
    palette.dark: Theme.chrome("Dark")
    palette.mid: Theme.chrome("Mid")
    palette.midlight: Theme.chrome("Midlight")
    palette.light: Theme.chrome("Light")
    palette.shadow: Theme.chrome("Shadow")
    palette.toolTipBase: Theme.chrome("ToolTipBase")
    palette.toolTipText: Theme.chrome("ToolTipText")
    palette.placeholderText: Theme.chrome("PlaceholderText")
    palette.link: Theme.chrome("Link")
    palette.accent: Theme.chrome("Accent")

    property bool sideOpen: false
    property string menuObject: ""
    property string removeName: ""
    property string removeDirectory: ""
    property string lastProject: ""
    property string noticeText: ""
    property var sideViews: []
    property int sideIndex: 0
    property string memoryObject: ""
    property string memoryRecordId: ""

    function popupAt(menu, origin) {
        const point = origin.mapToItem(Overlay.overlay, 0, origin.height)
        menu.fit()
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
            // The panel names a view from its kind when it shows it, so the
            // name follows the interface language.
            sideViews = sideViews.concat([{key: key, kind: kind, objectId: objectId || "",
                objectName: backend.selectedObject.name || ""}])
            sideIndex = sideViews.length - 1
        }
        sideOpen = true
        if (kind === "files") backend.refreshFiles()
    }
    function showSettings() {
        sharingCheck.checked = backend.sharing
        useMemoryCheck.checked = backend.useMemory
        generateMemoryCheck.checked = backend.generateMemory
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
    function confirmRemove(name, directory) {
        removeName = name
        removeDirectory = directory
        removeDialog.open()
    }
    function showContext(projectId, objectId, origin) {
        const point = origin.mapToItem(Overlay.overlay, 0, origin.height)
        if (!backend.selectProject(projectId)) return
        menuObject = objectId
        if (objectId) backend.selectObject(objectId)
        contextMenu.fit()
        contextMenu.x = Math.min(point.x, window.width - contextMenu.width - 12)
        contextMenu.y = point.y
        contextMenu.open()
    }

    Component.onCompleted: {
        Theme.backdrop = workspaceBackdrop
        Theme.basePointSize = appFont.pointSize > 0 ? appFont.pointSize : 10
        Theme.baseFont = appFont
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
        // Rings fading away from the edge, then the edge's hairline, as the
        // workbench frames its window.
        Repeater {
            model: windowFrame.shadowMargin
            Rectangle {
                required property int index
                readonly property real fade: 1 - index / Math.max(1, windowFrame.shadowMargin)
                anchors.fill: parent
                anchors.margins: -(index + 2)
                color: "transparent"
                border.width: 1
                border.color: Qt.rgba(Theme.windowShadow.r, Theme.windowShadow.g, Theme.windowShadow.b,
                                      Theme.windowShadow.a * fade * fade)
            }
        }
        Rectangle {
            visible: windowFrame.shadowMargin > 0
            anchors.fill: parent
            anchors.margins: -1
            color: "transparent"
            border.width: 1
            border.color: Theme.windowEdge
        }
    }
    Backdrop {
        id: workspaceBackdrop
        anchors.fill: shell
        cornerRadius: 0
    }
    // Two columns: the side list, and the conversation under the strip that
    // carries the project's name, its commands and the window's controls.
    RowLayout {
        anchors.fill: shell
        spacing: 0
        enabled: !startupError
        ProjectSidebar {
            Layout.preferredWidth: window.width < 1100 ? Theme.sidebarNarrowWidth : Theme.sidebarWidth
            Layout.fillHeight: true
            backend: controller
            windowActive: window.active
            onCreateRequested: createDialog.open()
            onOpenRequested: openProjectDialog.open()
            onSettingsRequested: window.showSettings()
            onMenuRequested: (projectId, objectId, origin) => window.showContext(projectId, objectId, origin)
            onUnavailableMenuRequested: (name, directory, origin) => {
                window.removeName = name
                window.removeDirectory = directory
                window.popupAt(unavailableMenu, origin)
            }
        }
        Rectangle { visible: Theme.highContrast; Layout.fillHeight: true; Layout.preferredWidth: 1; color: Theme.paneLine }
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.content
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Item {
                    id: titleBar
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.topBarHeight
                    WindowDrag { anchors.fill: parent }
                    Label {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.spaceLarge
                        anchors.right: titleActions.left
                        anchors.rightMargin: Theme.spaceMedium
                        anchors.verticalCenter: parent.verticalCenter
                        text: backend.activeProject.name || ""
                        color: window.active ? Theme.titleText : Theme.titleInactiveText
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    RowLayout {
                        id: titleActions
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        spacing: 2
                        readonly property bool inProject: backend.activeId.length > 0 && backend.previewTitle.length === 0
                        ActionButton {
                            visible: titleActions.inProject
                            text: window.width >= 1000 ? qsTr("Goal") : ""
                            glyph: "bookmark"
                            hint: qsTr("Set a persistent goal…")
                            checked: backend.goal.status === "active"
                            onClicked: goalEditor.open()
                        }
                        ActionButton {
                            visible: titleActions.inProject
                            glyph: "file_load"
                            hint: qsTr("Add analysis object…")
                            onClicked: objectFileDialog.open()
                        }
                        ActionButton {
                            visible: titleActions.inProject
                            glyph: "folder"
                            hint: qsTr("Project files")
                            onClicked: window.openSide("files", "")
                        }
                        ActionButton {
                            glyph: "comment"
                            readonly property bool waiting: backend.analysisObjectApprovals.some(request =>
                                request.projectId === backend.activeId && !!request.sideChatId)
                            hint: waiting ? qsTr("Side chat · Approval request pending") : qsTr("Side chat")
                            visible: backend.activeId.length > 0
                            onClicked: window.openSide("chat", "")
                            Rectangle {
                                visible: parent.waiting
                                anchors.right: parent.right; anchors.top: parent.top
                                anchors.margins: 5
                                width: 6; height: 6; radius: 3; color: Theme.accent
                            }
                        }
                        ActionButton {
                            id: panelToggle
                            glyph: "subviews"
                            hint: qsTr("Show or hide the sidebar · %1").arg(backend.shortcutText("Ctrl+B"))
                            checked: window.sideOpen
                            visible: backend.activeId.length > 0
                            Layout.rightMargin: Theme.spaceSmall
                            onClicked: {
                                if (window.sideOpen) window.sideOpen = false
                                else if (window.sideViews.length) window.sideOpen = true
                                else window.popupAt(viewMenu, this)
                            }
                        }
                        WindowControl { kind: "minimize"; frame: windowFrame; appWindow: window; Layout.fillHeight: true }
                        WindowControl { kind: "maximize"; frame: windowFrame; appWindow: window; Layout.fillHeight: true }
                        WindowControl { kind: "close"; frame: windowFrame; appWindow: window; Layout.fillHeight: true }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 0
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Conversation {
                            anchors.fill: parent
                            backend: controller
                            onCreateRequested: createDialog.open()
                            onOpenRequested: openProjectDialog.open()
                            onAddObjectRequested: objectFileDialog.open()
                            onFilesRequested: window.openSide("files", "")
                            onModelSettingsRequested: window.showModelSettings()
                            onGoalRequested: goalEditor.open()
                            onContextRequested: window.showContextPicker(false)
                        }
                        // What the application reports while it works, and
                        // what an action just did: a note that leaves again.
                        Rectangle {
                            id: notice
                            readonly property string message: backend.busy ? qsTr("Reading file fingerprints…") : window.noticeText
                            visible: message.length > 0
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: Theme.spaceSmall
                            width: Math.min(parent.width - 2 * Theme.spaceLarge, noticeLabel.implicitWidth + 2 * Theme.spaceMedium)
                            height: Theme.controlHeight
                            radius: height / 2
                            color: Theme.menu
                            border.width: 1
                            border.color: Theme.borderStrong
                            Label {
                                id: noticeLabel
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spaceMedium
                                anchors.rightMargin: Theme.spaceMedium
                                text: notice.message
                                color: Theme.menuText
                                font.pointSize: Theme.smallPointSize
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                Accessible.role: Accessible.StatusBar
                                Accessible.name: text
                            }
                            Timer {
                                id: noticeTimer
                                interval: 6000
                                onTriggered: window.noticeText = ""
                            }
                        }
                    }
                    Item {
                        id: sideSlot
                        visible: window.sideOpen && window.width >= 1100
                        Layout.preferredWidth: (window.sideViews[window.sideIndex] && ["evidence", "review", "research"].includes(window.sideViews[window.sideIndex].kind) ? 520 : window.sideViews[window.sideIndex] && window.sideViews[window.sideIndex].kind === "chat" ? 380 : 324) + Theme.spaceSmall
                        Layout.fillHeight: true
                    }
                }
            }
        }
    }
    SidePanel {
        id: details
        visible: window.sideOpen
        floating: window.width < 1100
        parent: window.width >= 1100 ? sideSlot : Overlay.overlay
        width: window.width >= 1100 ? parent.width - Theme.spaceSmall : Math.min(350, window.width - 32)
        height: window.width >= 1100 ? parent.height - Theme.spaceSmall : window.height - Theme.topBarHeight - 2 * windowFrame.shadowMargin - 16
        x: window.width >= 1100 ? 0 : window.width - width - windowFrame.shadowMargin - 10
        y: window.width >= 1100 ? 0 : windowFrame.shadowMargin + Theme.topBarHeight + 8
        backend: controller
        views: window.sideViews
        onContextRequested: window.showContextPicker(true)
        currentView: window.sideIndex
        onViewSelected: index => window.sideIndex = index
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
        ThemedMenuItem { text: qsTr("Open in the NeverD workbench"); visible: window.menuObject.length > 0; onTriggered: backend.openInGui(window.menuObject) }
        ThemedMenuItem {
            text: qsTr("Relocate file…")
            visible: window.menuObject.length > 0
            enabled: !backend.busy && !backend.reviewBusy
            onTriggered: {
                relocateFileDialog.projectId = backend.activeId
                relocateFileDialog.objectId = window.menuObject
                relocateFileDialog.open()
            }
        }
        ThemedMenuItem { text: qsTr("Show analysis progress in the sidebar"); visible: window.menuObject.length > 0; onTriggered: window.openSide("progress", window.menuObject) }
        ThemedMenuItem { text: qsTr("View object memory"); visible: window.menuObject.length > 0; onTriggered: window.openSide("memory", window.menuObject) }
        ThemedMenuItem { text: qsTr("Show project files in the sidebar"); visible: !window.menuObject; onTriggered: window.openSide("files", "") }
        ThemedMenuItem { text: qsTr("Open side chat"); onTriggered: window.openSide("chat", "") }
        ThemedMenuItem { text: qsTr("Add analysis object…"); visible: !window.menuObject; onTriggered: objectFileDialog.open() }
        ThemedSeparator { visible: !window.menuObject }
        ThemedMenuItem {
            text: qsTr("Remove project…")
            visible: !window.menuObject
            onTriggered: window.confirmRemove(backend.activeProject.name, backend.activeProject.directory)
        }
    }
    ThemedMenu {
        id: unavailableMenu
        ThemedMenuItem { text: qsTr("Try opening again"); onTriggered: backend.openProject(window.removeDirectory) }
        ThemedSeparator {}
        ThemedMenuItem { text: qsTr("Remove project…"); onTriggered: removeDialog.open() }
    }
    ThemedMenu {
        id: viewMenu
        ThemedMenuItem { text: qsTr("Project files"); onTriggered: window.openSide("files", "") }
        ThemedMenuItem { text: qsTr("Side chat"); onTriggered: window.openSide("chat", "") }
        ThemedMenuItem { text: qsTr("Analysis progress"); onTriggered: window.openSide("progress", backend.selectedObjectId) }
        ThemedMenuItem { text: qsTr("Research log"); onTriggered: window.openSide("research", "") }
        ThemedMenuItem { text: qsTr("Function evidence"); onTriggered: window.openSide("evidence", backend.selectedObjectId) }
        ThemedMenuItem { text: qsTr("Change review"); onTriggered: window.openSide("review", "") }
        ThemedMenuItem { text: qsTr("Object memory"); onTriggered: window.openSide("memory", backend.selectedObjectId) }
    }

    FolderDialog {
        id: openProjectDialog
        title: qsTr("Select an existing project folder")
        onAccepted: backend.openProject(selectedFolder.toString())
    }
    FolderDialog {
        id: createFolderDialog
        title: qsTr("Select the project's working directory")
        onAccepted: projectFolder.text = backend.pathFromUrl(selectedFolder.toString())
    }
    FileDialog {
        id: objectFileDialog
        title: qsTr("Add analysis object")
        fileMode: FileDialog.OpenFiles
        onAccepted: { for (const file of selectedFiles) backend.addObject(file.toString()) }
    }
    FileDialog {
        id: relocateFileDialog
        property string projectId
        property string objectId
        title: qsTr("Select the analysis object's file · SHA-256 will be verified")
        fileMode: FileDialog.OpenFile
        onAccepted: backend.relocateObject(projectId, objectId, selectedFile.toString())
    }
    FileDialog {
        id: guiFileDialog
        title: qsTr("Select the NeverD workbench executable")
        onAccepted: guiField.text = backend.pathFromUrl(selectedFile.toString())
    }
    FileDialog {
        id: memoryFileDialog
        property bool importing: true
        property string projectId
        property string objectId
        title: importing ? qsTr("Import object memory") : qsTr("Export object memory")
        fileMode: importing ? FileDialog.OpenFile : FileDialog.SaveFile
        nameFilters: [qsTr("Object memory (*.ndmemory.json)"), qsTr("JSON files (*.json)")]
        defaultSuffix: "ndmemory.json"
        onAccepted: backend.transferMemory(projectId, objectId, selectedFile.toString(), importing)
    }

    component Sheet: Dialog {
        anchors.centerIn: parent
        parent: Overlay.overlay
        width: Math.min(560, window.width - 48)
        modal: true
        padding: Theme.dialogPadding
        topPadding: Theme.spaceMedium
        closePolicy: Popup.CloseOnEscape
        Overlay.modal: ModalDim {}
        background: Surface { glass: true; radius: Theme.radius; color: Theme.background }
        header: Label {
            text: parent.title
            // A title may carry a name the person chose: plain text, and
            // a second line before it is cut.
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            color: Theme.text
            font.pointSize: Theme.titlePointSize
            font.weight: Font.Medium
            padding: Theme.dialogPadding
            bottomPadding: 0
        }
    }
    Sheet {
        id: removeDialog
        title: qsTr("Remove %1?").arg(window.removeName)
        onOpened: removeButton.forceActiveFocus()
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                text: qsTr("This only takes the project off the list. Nothing on disk is deleted: its folder, conversation and memory stay where they are, and Open project brings it back.")
                color: Theme.text
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            Label {
                text: window.removeDirectory
                textFormat: Text.PlainText
                color: Theme.secondary
                Layout.fillWidth: true
                // A path reads left to right; it still starts where the
                // sentence above it does.
                horizontalAlignment: Text.AlignLeft
                elide: Text.ElideMiddle
            }
            RowLayout {
                Layout.topMargin: 10
                Item { Layout.fillWidth: true }
                ActionButton { text: qsTr("Cancel"); onClicked: removeDialog.close() }
                ActionButton {
                    id: removeButton
                    text: qsTr("Remove project")
                    primary: true
                    onClicked: {
                        // A refusal says why in its own sheet.
                        removeDialog.close()
                        backend.removeProject(window.removeDirectory)
                    }
                }
            }
        }
    }
    Sheet {
        id: createDialog
        title: qsTr("New project")
        contentItem: ColumnLayout {
            spacing: 12
            Label { text: qsTr("Project name"); color: Theme.secondary }
            Field { id: projectName; placeholderText: qsTr("e.g. Windows API research"); Layout.fillWidth: true }
            Label { text: qsTr("Project folder"); color: Theme.secondary }
            RowLayout {
                Layout.fillWidth: true
                Field { id: projectFolder; placeholderText: qsTr("Holds notes, reports and project memory"); Layout.fillWidth: true }
                ActionButton { glyph: "open"; text: qsTr("Browse…"); onClicked: createFolderDialog.open() }
            }
            Label {
                text: qsTr("A .nd-agent folder is created in this directory to hold this project's own context. Analysis objects can be in other directories.")
                color: Theme.secondary
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.topMargin: 10
                Item { Layout.fillWidth: true }
                ActionButton { text: qsTr("Cancel"); onClicked: createDialog.close() }
                ActionButton {
                    text: qsTr("Create project")
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
        title: qsTr("Application settings")
        contentItem: ColumnLayout {
            spacing: 14
            ActionButton {
                text: qsTr("Model and analysis settings…")
                glyph: "options"
                Layout.fillWidth: true
                onClicked: { settingsDialog.close(); modelSettingsDialog.open() }
            }
            Label {
                text: qsTr("Language and theme follow the NeverD workbench: Options ▸ Language and Options ▸ Colors.")
                color: Theme.secondary
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Analysis objects added by AI"); color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
                ChoiceField {
                    id: objectApprovalMode
                    model: [qsTr("Require approval each time"), qsTr("Allow automatically")]
                    Layout.preferredWidth: Math.min(fittingWidth, settingsDialog.availableWidth * 0.6)
                    Accessible.name: qsTr("Analysis objects added by AI")
                }
            }
            CheckField {
                id: useMemoryCheck
                text: qsTr("Recall saved memory during analysis")

            }
            CheckField {
                id: generateMemoryCheck
                text: qsTr("Organize long-term memory automatically when idle")

            }
            Label {
                text: qsTr("Organizing uses the current model and incurs additional API usage. It processes only completed exchanges and their evidence references, and pauses when a new analysis starts. Turning off recall does not delete saved memory.")
                color: Theme.secondary
                wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize
                Layout.fillWidth: true
            }
            CheckField {
                id: sharingCheck
                text: qsTr("Share analysis object memory across projects")

            }
            Label {
                text: qsTr("Reads object memory from other registered projects by file fingerprint. Project conversations stay separate, and memory from other projects is read-only.")
                color: Theme.secondary
                wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize
                Layout.fillWidth: true
            }
            Label { text: qsTr("NeverD workbench executable"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
            RowLayout {
                Layout.fillWidth: true
                Field { id: guiField; placeholderText: qsTr("Leave empty to search the adjacent directory and PATH"); Layout.fillWidth: true }
                ActionButton { glyph: "open"; hint: qsTr("Choose executable…"); onClicked: guiFileDialog.open() }
            }
            Label { text: qsTr("Global data directory"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
            Label { text: backend.dataDirectory; color: Theme.secondary; wrapMode: Text.WrapAnywhere; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            RowLayout {
                Layout.topMargin: 8
                Item { Layout.fillWidth: true }
                ActionButton { text: qsTr("Cancel"); onClicked: settingsDialog.close() }
                ActionButton {
                    text: qsTr("Save settings")
                    primary: true
                    onClicked: {
                        if (backend.updateWorkspaceSettings(sharingCheck.checked, backend.lightTheme,
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
        title: window.memoryRecordId ? qsTr("Edit and confirm memory") : window.memoryObject ? qsTr("Add object memory") : qsTr("Add project memory")
        contentItem: ColumnLayout {
            spacing: 12
            Label { text: qsTr("Record a confirmed fact and its evidence source."); color: Theme.secondary; Layout.fillWidth: true }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 220
                TextArea {
                    id: memoryText
                    color: Theme.text
                    placeholderTextColor: Theme.quiet
                    placeholderText: qsTr("e.g. key functions, call relationships, addresses and how they were verified…")
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    background: Rectangle { color: Theme.raised; radius: Theme.controlRadius; border.width: 1; border.color: memoryText.activeFocus ? Theme.focusBorder : Theme.inputBorder }
                }
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                ActionButton { text: qsTr("Cancel"); onClicked: memoryDialog.close() }
                ActionButton {
                    text: qsTr("Save memory"); primary: true; enabled: memoryText.text.trim().length > 0
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
        title: qsTr("Operation not completed")
        closePolicy: startupError ? Popup.NoAutoClose : Popup.CloseOnEscape
        contentItem: ColumnLayout {
            spacing: 16
            Label { id: failureText; color: Theme.text; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            ActionButton {
                text: startupError ? qsTr("Quit") : qsTr("OK")
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
        function onNotice(message) { window.noticeText = message; noticeTimer.restart() }
        function onOperationError(message) { failureText.text = message; failureDialog.open() }
        function onWorkspaceChanged() {
            if (window.lastProject !== backend.activeId) {
                window.lastProject = backend.activeId
                window.sideViews = []
                window.sideIndex = 0
                // The panel shows a project's views; with none open it closes.
                if (!backend.activeId) window.sideOpen = false
                else if (window.sideOpen) window.openSide("files", "")
            }
        }
    }
}
