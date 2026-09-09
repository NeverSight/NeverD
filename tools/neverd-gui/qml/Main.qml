pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore

ApplicationWindow {
    id: window
    objectName: "neverdWorkbench"
    visible: true
    width: preferences.windowWidth
    height: preferences.windowHeight
    minimumWidth: 900
    minimumHeight: 620
    title: workbench.fileName ? workbench.fileName + " — NeverD" : qsTr("NeverD — Binary Analysis")
    color: Theme.editor
    font.pointSize: Theme.bodySize
    palette.window: Theme.editor
    palette.windowText: Theme.foreground
    palette.base: Theme.editor
    palette.alternateBase: Theme.sidebar
    palette.text: Theme.foreground
    palette.button: Theme.elevated
    palette.buttonText: Theme.foreground
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentForeground
    palette.mid: Theme.border
    palette.dark: Theme.editor
    LayoutMirroring.enabled: workbench.language === "ar"
    LayoutMirroring.childrenInherit: true
    readonly property bool interfaceMirrored: LayoutMirroring.enabled
    readonly property bool textEntryActive: activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit
    readonly property var representationIds: ["c", "low", "med", "high", "llvm"]
    readonly property var languageCodes: ["en", "zh-CN", "zh-TW", "ja", "ko", "fr", "de", "es", "it", "ru", "ar"]
    readonly property var languageNames: ["English", "简体中文", "繁體中文", "日本語", "한국어", "Français", "Deutsch", "Español", "Italiano", "Русский", "العربية"]

    Settings {
        id: preferences
        category: "Workbench"
        property int windowWidth: 1500
        property int windowHeight: 950
        property real codePointSize: 11
    }
    property bool closeApproved: false
    onClosing: close => {
        if (!closeApproved && !workbench.requestClose()) { close.accepted = false; return }
        preferences.windowWidth = width
        preferences.windowHeight = height
        dockWorkspace.saveLayout()
    }

    Connections {
        target: workbench
        function onConfirmSessionChange() { unsavedDialog.open() }
        function onCloseReady() { window.closeApproved = true; window.close() }
    }
    Dialog {
        id: unsavedDialog
        objectName: "unsavedAnnotationsDialog"
        width: Math.min(440, window.width - 40)
        title: qsTr("Unsaved annotations")
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.NoAutoClose
        standardButtons: Dialog.Save | Dialog.Discard | Dialog.Cancel
        onAccepted: workbench.resolveSessionChange("save")
        onDiscarded: workbench.resolveSessionChange("discard")
        onRejected: workbench.resolveSessionChange("cancel")
        contentItem: Label {
            text: qsTr("Save annotation changes before continuing?")
            color: Theme.foreground
            wrapMode: Text.WordWrap
        }
    }

    Action { id: openAction; text: qsTr("Open Binary…"); shortcut: StandardKey.Open; onTriggered: fileDialog.open() }
    Action { id: navigateAction; text: qsTr("Go to Address or Symbol…"); shortcut: "G"; enabled: workbench.loaded && !window.textEntryActive; onTriggered: { addressField.forceActiveFocus(); addressField.selectAll() } }
    Action { id: backAction; text: qsTr("Back"); shortcut: StandardKey.Back; enabled: workbench.canGoBack; onTriggered: workbench.goBack() }
    Action { id: forwardAction; text: qsTr("Forward"); shortcut: StandardKey.Forward; enabled: workbench.canGoForward; onTriggered: workbench.goForward() }
    Action { id: settingsAction; text: qsTr("Settings…"); shortcut: StandardKey.Preferences; onTriggered: settingsDialog.open() }
    Action { id: renameAction; text: qsTr("Rename Function…"); shortcut: "N"; enabled: workbench.loaded && !window.textEntryActive; onTriggered: window.openRename() }
    Action { id: commentAction; text: qsTr("Edit Comment…"); shortcut: ";"; enabled: workbench.loaded && !window.textEntryActive; onTriggered: window.openComment() }
    Action { id: undoAction; text: qsTr("Undo"); shortcut: StandardKey.Undo; enabled: workbench.canUndo && !window.textEntryActive; onTriggered: workbench.undo() }
    Action { id: redoAction; text: qsTr("Redo"); shortcut: StandardKey.Redo; enabled: workbench.canRedo && !window.textEntryActive; onTriggered: workbench.redo() }
    Action { id: cancelAction; text: qsTr("Cancel Analysis"); enabled: workbench.busy; onTriggered: workbench.cancel() }
    Action { id: restartAction; text: qsTr("Restart Worker"); onTriggered: workbench.restartWorker() }
    Action { id: toggleOutputAction; text: qsTr("Toggle Bottom Panel"); shortcut: "Ctrl+J"; onTriggered: dockWorkspace.toggleBottom() }
    Action { id: quitAction; text: qsTr("Quit NeverD"); shortcut: StandardKey.Quit; onTriggered: window.close() }

    menuBar: MenuBar {
        background: Rectangle { color: Theme.sidebar }
        delegate: MenuBarItem {
            id: menuItem
            contentItem: Text { textFormat: Text.PlainText; text: menuItem.text; color: Theme.foreground; font.pointSize: Theme.bodySize; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: menuItem.highlighted ? Theme.hover : "transparent" }
        }
        Menu { title: qsTr("File"); Action { text: openAction.text; shortcut: openAction.shortcut; onTriggered: openAction.trigger() } MenuSeparator {} MenuItem { action: settingsAction } MenuSeparator {} MenuItem { action: quitAction } }
        Menu { title: qsTr("Navigate"); MenuItem { action: backAction } MenuItem { action: forwardAction } MenuSeparator {} MenuItem { action: navigateAction } }
        Menu { title: qsTr("Edit"); MenuItem { action: undoAction } MenuItem { action: redoAction } MenuSeparator {} MenuItem { action: renameAction } MenuItem { action: commentAction } MenuSeparator {} Action { text: qsTr("Save Annotations"); enabled: workbench.loaded; onTriggered: workbench.saveAnnotations() } Action { text: qsTr("Reload Annotations"); enabled: workbench.loaded; onTriggered: workbench.loadAnnotations() } }
        Menu {
            title: qsTr("View")
            MenuItem { action: toggleOutputAction }
            Action { text: qsTr("Disassembly"); onTriggered: window.switchCenter(0) }
            Action { text: qsTr("Control Flow"); onTriggered: window.switchCenter(1) }
            Action { text: qsTr("Hex"); onTriggered: window.switchCenter(2) }
            Action { text: qsTranslate("FunctionsPane", "FUNCTIONS"); onTriggered: dockWorkspace.showPanel("functions") }
            Action { text: qsTr("Pseudocode & intermediate representations"); onTriggered: dockWorkspace.showPanel("representation") }
            Action { text: qsTr("References"); onTriggered: dockWorkspace.showPanel("references") }
            Action { text: qsTr("Output"); onTriggered: dockWorkspace.showPanel("output") }
            Action { text: qsTr("Connections"); onTriggered: dockWorkspace.showPanel("connections") }
            Menu {
                title: qsTr("Focus Panel")
                Action { text: qsTr("Disassembly"); onTriggered: dockWorkspace.focusPanel("machine") }
                Action { text: qsTr("Pseudocode & intermediate representations"); onTriggered: dockWorkspace.focusPanel("representation") }
            }
            MenuSeparator {}
            Action { text: qsTr("Reset Layout"); onTriggered: dockWorkspace.resetLayout() }
        }
        Menu { title: qsTr("Analysis"); MenuItem { action: cancelAction } MenuItem { action: restartAction } }
        Menu {
            title: qsTr("Extensions")
            Action { text: qsTr("Import Manifest…"); onTriggered: extensionFileDialog.open() }
            Action { text: qsTr("Extensions"); onTriggered: dockWorkspace.showPanel("extensions") }
        }
        Menu { title: qsTr("Help"); Action { text: qsTr("Keyboard Shortcuts"); onTriggered: shortcutsDialog.open() } Action { text: qsTr("About NeverD"); onTriggered: aboutDialog.open() } }
    }

    header: Rectangle {
        height: 78
        color: Theme.sidebar
        ColumnLayout {
            anchors.fill: parent
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                Layout.leftMargin: 9
                Layout.rightMargin: 12
                spacing: 6
                Image {
                    objectName: "neverdBrandLogo"
                    source: "qrc:/brand/neverd-logo-dark.svg"
                    sourceSize: Qt.size(32, 32)
                    fillMode: Image.PreserveAspectFit
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    Layout.leftMargin: 7
                    Layout.rightMargin: 8
                    Accessible.name: "NeverD"
                    Accessible.role: Accessible.Graphic
                }
                WorkbenchButton { text: qsTr("Open Binary"); hint: openAction.text + " (" + openAction.shortcut + ")"; primary: !workbench.loaded; onClicked: fileDialog.open() }
                Rectangle { width: 1; Layout.preferredHeight: 20; color: Theme.border; Layout.margins: 5 }
                WorkbenchButton { text: "←"; hint: backAction.text; enabled: backAction.enabled; onClicked: workbench.goBack(); rotation: workbench.language === "ar" ? 180 : 0 }
                WorkbenchButton { text: "→"; hint: forwardAction.text; enabled: forwardAction.enabled; onClicked: workbench.goForward(); rotation: workbench.language === "ar" ? 180 : 0 }
                WorkbenchField {
                    id: addressField
                    objectName: "addressField"
                    readonly property bool actualMirrored: LayoutMirroring.enabled
                    Layout.fillWidth: true
                    Layout.maximumWidth: 560
                    Layout.minimumWidth: 160
                    placeholderText: qsTr("Go to address or symbol…")
                    Accessible.name: qsTr("Address or symbol")
                    font.family: Theme.monoFont
                    LayoutMirroring.enabled: false
                    horizontalAlignment: TextInput.AlignLeft
                    onAccepted: { if (text.trim()) workbench.navigate(text.trim()); focus = false }
                    Keys.onEscapePressed: { clear(); focus = false }
                }
                Item { Layout.fillWidth: true; visible: window.width > 1200 }
                Text { textFormat: Text.PlainText; text: workbench.architecture || ""; color: Theme.muted; font.pointSize: Theme.captionSize; visible: window.width > 1020 }
                WorkbenchButton { text: qsTr("Cancel"); visible: workbench.busy; onClicked: workbench.cancel() }
                WorkbenchButton { text: qsTr("Settings"); hint: qsTr("Language and editor preferences"); onClicked: settingsDialog.open() }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 16
                Layout.rightMargin: 14
                spacing: 11
                Text { textFormat: Text.PlainText; text: workbench.loaded ? workbench.fileName : qsTr("WORKSPACE"); color: workbench.loaded ? Theme.foreground : Theme.subdued; font.pointSize: Theme.captionSize; elide: Text.ElideMiddle; Layout.maximumWidth: window.width * 0.35 }
                Text { textFormat: Text.PlainText; text: "›"; color: Theme.subdued; visible: workbench.loaded }
                Text { textFormat: Text.PlainText; text: workbench.selectedFunctionName || qsTr("No binary open"); color: workbench.loaded ? Theme.functionName : Theme.muted; font.pointSize: Theme.captionSize; elide: Text.ElideRight; Layout.fillWidth: true; LayoutMirroring.enabled: false }
                Text { textFormat: Text.PlainText; text: workbench.format || ""; color: Theme.subdued; font.pointSize: Theme.captionSize }
                Text { textFormat: Text.PlainText; text: workbench.loaded ? qsTr("%1 functions").arg(workbench.functionCount) : qsTr("Local analysis"); color: Theme.subdued; font.pointSize: Theme.captionSize }
            }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            visible: workbench.error.length > 0
            Layout.fillWidth: true
            implicitHeight: errorLabel.implicitHeight + 20
            color: Theme.sidebar
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 10
                Text { textFormat: Text.PlainText; text: "!"; color: Theme.error; font.bold: true }
                Text { textFormat: Text.PlainText; id: errorLabel; text: workbench.error; color: Theme.error; font.pointSize: Theme.bodySize; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                WorkbenchButton { text: qsTr("Restart Worker"); onClicked: workbench.restartWorker() }
            }
        }
        DockWorkspace {
            id: dockWorkspace
            Layout.fillWidth: true
            Layout.fillHeight: true
            controller: workbench
            client: mcp
            broker: sessionBroker
            codePointSize: preferences.codePointSize
            onRenameRequested: window.openRename()
            onCommentRequested: window.openComment()
            onImportExtensionsRequested: extensionFileDialog.open()
        }
    }

    footer: Rectangle {
        height: 26
        color: Theme.accent
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 13
            anchors.rightMargin: 12
            spacing: 17
            Text { textFormat: Text.PlainText; text: workbench.workerConnected ? qsTr("Worker connected") : qsTr("Worker offline"); color: Theme.accentForeground; font.pointSize: Theme.captionSize }
            Rectangle { implicitWidth: 1; implicitHeight: 12; color: Theme.accentForegroundMuted }
            Text { textFormat: Text.PlainText; text: workbench.status; color: Theme.accentForeground; font.pointSize: Theme.captionSize; elide: Text.ElideRight; Layout.fillWidth: true }
            ProgressBar {
                id: analysisProgress
                visible: workbench.busy
                Layout.preferredWidth: 110
                Layout.preferredHeight: 3
                value: workbench.progress
                indeterminate: workbench.progress < 0
                background: Rectangle { color: Theme.accentTrack; radius: 1 }
                contentItem: Item {
                    Rectangle {
                        width: parent.width * (analysisProgress.indeterminate ? 1 : analysisProgress.visualPosition)
                        height: parent.height
                        color: Theme.accentForeground
                        SequentialAnimation on opacity {
                            running: analysisProgress.visible && analysisProgress.indeterminate
                            loops: Animation.Infinite
                            NumberAnimation { from: 0.35; to: 1; duration: 650 }
                            NumberAnimation { from: 1; to: 0.35; duration: 650 }
                        }
                    }
                }
            }
            Text { textFormat: Text.PlainText; text: workbench.selectedAddress || ""; color: Theme.accentForeground; font.family: Theme.monoFont; font.pointSize: Theme.captionSize; LayoutMirroring.enabled: false }
            Text { textFormat: Text.PlainText; text: window.languageNames[Math.max(0, window.languageCodes.indexOf(workbench.language))]; color: Theme.accentForeground; font.pointSize: Theme.captionSize; visible: window.width > 1100 }
        }
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Open Binary")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("All files (*)")]
        onAccepted: workbench.openFile(selectedFile)
    }
    FileDialog {
        id: extensionFileDialog
        title: qsTr("Import Manifest")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("JSON files (*.json)"), qsTr("All files (*)")]
        onAccepted: { workbench.importContributions(selectedFile); dockWorkspace.showPanel("extensions") }
    }
    function switchCenter(index) {
        dockWorkspace.selectMachineView(index)
    }
    function openRename() { renameField.text = workbench.selectedFunctionName; renameDialog.open(); renameField.forceActiveFocus(); renameField.selectAll() }
    function openComment() { commentField.text = workbench.selectedComment; commentDialog.open(); commentField.forceActiveFocus() }

    Dialog {
        id: renameDialog
        title: qsTr("Rename Function")
        anchors.centerIn: parent
        width: Math.min(460, window.width - 40)
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        background: Rectangle { color: Theme.sidebar; border.color: Theme.border; radius: 4 }
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            Text { textFormat: Text.PlainText; text: workbench.selectedAddress; color: Theme.subdued; font.family: Theme.monoFont; font.pointSize: Theme.bodySize }
            WorkbenchField { id: renameField; Layout.fillWidth: true; placeholderText: qsTr("Function name"); Accessible.name: qsTr("Function name"); onAccepted: { workbench.renameFunction(text); renameDialog.close() } }
        }
        onAccepted: workbench.renameFunction(renameField.text)
    }
    Dialog {
        id: commentDialog
        title: qsTr("Edit Comment")
        anchors.centerIn: parent
        width: Math.min(580, window.width - 40)
        modal: true
        standardButtons: Dialog.Save | Dialog.Cancel
        background: Rectangle { color: Theme.sidebar; border.color: Theme.border; radius: 4 }
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            Text { textFormat: Text.PlainText; text: workbench.selectedAddress; color: Theme.subdued; font.family: Theme.monoFont; font.pointSize: Theme.bodySize; LayoutMirroring.enabled: false }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 180
                TextArea {
                    id: commentField
                    color: Theme.foreground
                    font.pointSize: Theme.bodySize
                    placeholderText: qsTr("Add a comment for this address…")
                    placeholderTextColor: Theme.subdued
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    background: Rectangle { color: Theme.editor; border.color: commentField.activeFocus ? Theme.focus : Theme.border }
                    Accessible.name: qsTr("Comment")
                }
            }
        }
        onAccepted: workbench.setComment(commentField.text)
    }
    Dialog {
        id: settingsDialog
        objectName: "settingsDialog"
        title: qsTr("Settings")
        anchors.centerIn: parent
        width: Math.min(520, window.width - 40)
        modal: true
        standardButtons: Dialog.Close
        background: Rectangle { color: Theme.sidebar; border.color: Theme.border; radius: 4 }
        ColumnLayout {
            anchors.fill: parent
            spacing: 18
            Text { textFormat: Text.PlainText; text: qsTr("Appearance"); color: Theme.foreground; font.pointSize: Theme.bodySize; font.weight: Font.Medium }
            RowLayout {
                Layout.fillWidth: true
                Text { textFormat: Text.PlainText; text: qsTr("Theme"); color: Theme.muted; font.pointSize: Theme.bodySize; Layout.fillWidth: true }
                Text { textFormat: Text.PlainText; text: "Dark+"; color: Theme.foreground; font.pointSize: Theme.bodySize }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { textFormat: Text.PlainText; text: qsTr("Code font size"); color: Theme.muted; font.pointSize: Theme.bodySize; Layout.fillWidth: true }
                SpinBox { from: 9; to: 20; value: preferences.codePointSize; editable: true; onValueModified: preferences.codePointSize = value }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
            Text { textFormat: Text.PlainText; text: qsTr("Language"); color: Theme.foreground; font.pointSize: Theme.bodySize; font.weight: Font.Medium }
            ComboBox {
                id: languageSelector
                objectName: "languageSelector"
                Layout.fillWidth: true
                model: window.languageNames
                currentIndex: Math.max(0, window.languageCodes.indexOf(workbench.language))
                onActivated: index => workbench.setLanguage(window.languageCodes[index])
                Accessible.name: qsTr("Interface language")
            }
            Text { textFormat: Text.PlainText; text: qsTr("Changes apply immediately. Code, symbols, and comments keep their original language."); color: Theme.muted; font.pointSize: Theme.bodySize; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
    }
    Dialog {
        id: shortcutsDialog
        title: qsTr("Keyboard Shortcuts")
        anchors.centerIn: parent
        width: 420
        modal: true
        standardButtons: Dialog.Close
        background: Rectangle { color: Theme.sidebar; border.color: Theme.border; radius: 4 }
        Label { text: qsTr("Open binary: %1\nGo to address or symbol: G\nRename function: N\nEdit comment: ;\nToggle bottom panel: Ctrl+J\nBack / forward: %2 / %3").arg(openAction.shortcut).arg(backAction.shortcut).arg(forwardAction.shortcut); color: Theme.foreground; font.pointSize: Theme.bodySize; lineHeight: 1.8 }
    }
    Dialog {
        id: aboutDialog
        title: qsTr("About NeverD")
        anchors.centerIn: parent
        width: 420
        modal: true
        standardButtons: Dialog.Close
        background: Rectangle { color: Theme.sidebar; border.color: Theme.border; radius: 4 }
        Label { text: qsTr("NeverD\nBinary analysis workbench\n\nExplore disassembly, control flow, recovered C, and intermediate representations with a local analysis worker."); width: parent.width; wrapMode: Text.WordWrap; color: Theme.foreground; font.pointSize: Theme.bodySize; lineHeight: 1.4 }
    }
}
