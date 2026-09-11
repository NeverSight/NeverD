pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import NeverD.Native 1.0
import com.kdab.dockwidgets 2.0 as Dock

Rectangle {
    id: root
    objectName: "dockWorkspace"
    required property var controller
    required property var client
    required property var broker
    property var focusedItem: null
    property string focusedPanel: ""
    property int focusEpoch: 0
    readonly property var registry: controller.panes
    readonly property var machineController: registry.defaultMachine
    readonly property var representationController: registry.defaultRepresentation
    property string lastAnalysisPaneId: ""
    property int layoutMutationDepth: 0
    property int layoutEpoch: 0
    property bool layoutSettling: true
    property bool synchronizingPanes: false
    readonly property bool machineContentVisible: contentIsVisible(machine)
    readonly property bool representationContentVisible: contentIsVisible(representation)
    onMachineContentVisibleChanged: schedulePaneSync()
    onRepresentationContentVisibleChanged: schedulePaneSync()
    onMachineActiveChanged: synchronizeActivePane()
    onRepresentationActiveChanged: synchronizeActivePane()
    readonly property bool machineActive: containsFocus(machine)
    readonly property bool representationActive: containsFocus(representation)
    readonly property bool analysisContentActive: (machineActive || representationActive) && focusedItem && (focusedItem.objectName === "disassemblyList" || focusedItem.objectName === "instructionRow" || focusedItem.objectName === "graphViewport" || focusedItem.objectName === "codeText")
    property real codePointSize: Theme.codeSize
    property bool initialized: false
    signal renameRequested()
    signal commentRequested()
    signal importExtensionsRequested()
    signal connectionDialogRequested(var dialog, var focusItem)
    color: Theme.editor
    readonly property var panels: [functionsDock, machineDock, representationDock, referencesDock, outputDock, connectionsDock, extensionsDock]
    readonly property var contents: [functions, machine, representation, references, output, connections, extensions]
    function containsFocus(item) {
        for (let cursor = focusedItem; cursor; cursor = cursor.parent)
            if (cursor === item) return true
        return false
    }
    function contentIsVisible(item) {
        if (!item) return false
        const targetWindow = item.Window.window
        return item.visible && targetWindow && targetWindow.visible &&
                targetWindow.visibility !== Window.Minimized
    }
    function analysisController(index) {
        return index === 1 ? machineController : index === 2 ? representationController : null
    }
    function synchronizeActivePane() {
        if (!initialized || layoutSettling || layoutMutationDepth > 0 || synchronizingPanes) return
        const pane = machineActive && machineContentVisible ? machineController :
                     representationActive && representationContentVisible ? representationController : null
        if (!pane || !pane.open) return
        lastAnalysisPaneId = pane.id
        registry.setActivePane(pane.id)
    }
    function schedulePaneSync() { Qt.callLater(synchronizeAnalysisPanes) }
    function synchronizeAnalysisPanes() {
        if (!initialized || layoutSettling || layoutMutationDepth > 0 || synchronizingPanes) return
        synchronizingPanes = true
        try {
            for (const index of [1, 2]) {
                const pane = analysisController(index)
                const panel = panels[index]
                // Focus Panel hides peers temporarily without changing their
                // membership/open intent. Actual visibility still gates reads.
                if (focusedPanel.length === 0 || panel.uniqueName === "neverd." + focusedPanel)
                    registry.setPaneOpen(pane.id, panel.isOpen)
                registry.setPaneVisible(pane.id, panel.isOpen && contentIsVisible(contents[index]))
            }
        } finally {
            synchronizingPanes = false
        }
        synchronizeActivePane()
    }
    function beginLayoutChange() {
        ++layoutMutationDepth
        ++layoutEpoch
        layoutSettling = true
    }
    function endLayoutChange() {
        --layoutMutationDepth
        if (layoutMutationDepth !== 0) return
        const epoch = layoutEpoch
        Qt.callLater(() => {
            if (epoch !== layoutEpoch || layoutMutationDepth !== 0) return
            layoutSettling = false
            synchronizeAnalysisPanes()
        })
    }
    function cancelPendingFocus() { ++focusEpoch }
    function requestFocus(index, search) {
        const epoch = ++focusEpoch
        const panel = panels[index]
        const item = contents[index]
        Qt.callLater(() => {
            // A restored layout may move this item into a different window.
            // Only the latest request may activate it after the layout settles.
            if (epoch !== focusEpoch || !panel || !item ||
                    panels[index] !== panel || contents[index] !== item ||
                    !panel.isOpen || !item.visible) return
            focusContents(index, search)
        })
    }
    function focusContents(index, search) {
        const item = contents[index]
        const targetWindow = item.Window.window
        if (targetWindow) { targetWindow.raise(); targetWindow.requestActivate() }
        if (search && typeof item.focusSearch === "function") item.focusSearch()
        else if (typeof item.focusContent === "function") item.focusContent()
        else item.forceActiveFocus(Qt.ShortcutFocusReason)
    }
    function resetLayout() {
        beginLayoutChange()
        try {
            cancelPendingFocus()
            focusedPanel = ""

            for (const panel of panels) {
                panel.isFloating = false
                panel.forceClose()
            }
            // Insert the sidebar last so later analysis panes cannot squeeze
            // its initial width down to the minimum allowed for saved layouts.
            area.addDockWidget(machineDock, Dock.KDDockWidgets.Location_OnLeft, null, Qt.size(width, height - 180))
            area.addDockWidget(representationDock, width < 1150 ? Dock.KDDockWidgets.Location_OnBottom : Dock.KDDockWidgets.Location_OnRight, machineDock, Qt.size(Math.max(360, (width - 5) / 2), Math.max(230, height / 2)))
            area.addDockWidget(functionsDock, Dock.KDDockWidgets.Location_OnLeft, null, Qt.size(280, height - 180))
            area.addDockWidget(referencesDock, Dock.KDDockWidgets.Location_OnBottom, null, Qt.size(width, 180))
            referencesDock.addDockWidgetAsTab(outputDock)
            referencesDock.addDockWidgetAsTab(connectionsDock)
            referencesDock.addDockWidgetAsTab(extensionsDock, Dock.KDDockWidgets.StartHidden)
            referencesDock.setAsCurrentTab()
        } finally {
            endLayoutChange()
        }
    }
    function saveLayout() {
        // KDDW may close dock windows before ApplicationWindow.closing runs.
        // Preserve the last usable layout instead of persisting that teardown.
        if (initialized && !layoutSettling && layoutMutationDepth === 0 && focusedPanel.length === 0 && panels.some(panel => panel.isOpen)) saver.saveToFile(controller.dockLayoutPath)
    }
    function showPanel(name) { showPanelWithFocus(name, false) }
    function showPanelWithFocus(name, search, focusResult = true) {
        beginLayoutChange()
        try {
            cancelPendingFocus()
            if (focusedPanel.length > 0 && focusedPanel !== name) restoreFocusedLayout()
            for (let index = 0; index < panels.length; ++index) {
                const panel = panels[index]
                if (panel.uniqueName === "neverd." + name) {
                    panel.show()
                    panel.raise()
                    panel.setAsCurrentTab()
                    // An explicit Show is an open intent; navigation may follow
                    // synchronously before the delayed guest-focus callback.
                    const pane = analysisController(index)
                    if (pane) registry.setPaneOpen(pane.id, true)
                    if (focusResult) requestFocus(index, search)
                    return
                }
            }
        } finally {
            endLayoutChange()
        }
    }
    function navigateTo(query, focusResult = true) {
        const input = query.trim()
        if (!input) return
        const index = lastAnalysisPaneId === representationController.id ? 2 : 1
        const pane = analysisController(index)
        showPanelWithFocus(index === 2 ? "representation" : "machine", false, focusResult)
        registry.setPaneOpen(pane.id, true)
        lastAnalysisPaneId = pane.id
        registry.setActivePane(pane.id)
        pane.navigate(input)
        // Showing a result may activate its native window. Restore Functions
        // through the same latest-only request after the layout settles.
        if (!focusResult) requestFocus(0, false)
    }
    function selectMachineView(index) {
        showPanel("machine")
        machine.selectView(index)
    }
    function toggleMachineView() { selectMachineView(machine.currentIndex === 1 ? 0 : 1) }
    function toggleAnalysisFocus() { showPanel(representationActive ? "machine" : "representation") }
    function focusFunctionSearch() { showPanelWithFocus("functions", true) }
    function cyclePanel(direction) {
        const focused = contents.findIndex(item => containsFocus(item))
        const current = focused < 0 && direction < 0 ? 0 : focused
        for (let step = 1; step <= panels.length; ++step) {
            const index = (current + direction * step + panels.length * 2) % panels.length
            if (!panels[index].isOpen) continue
            showPanel(panels[index].uniqueName.substring("neverd.".length))
            return
        }
    }
    function toggleBottom() {
        cancelPendingFocus()
        const visible = referencesDock.isOpen || outputDock.isOpen || connectionsDock.isOpen || extensionsDock.isOpen
        if (visible) {
            referencesDock.close()
            outputDock.close()
            connectionsDock.close()
            extensionsDock.close()
        } else {
            referencesDock.show()
            outputDock.show()
            connectionsDock.show()
            referencesDock.setAsCurrentTab()
        }
    }
    function focusPanel(name) {
        beginLayoutChange()
        try {
            cancelPendingFocus()
            if (focusedPanel === name) { restoreFocusedLayout(); return }
            if (focusedPanel.length > 0) restoreFocusedLayout()
            if (!saver.saveToFile(controller.dockLayoutPath)) return
            focusedPanel = name
            for (const panel of panels) {
                if (panel.uniqueName !== "neverd." + name) panel.close()
                else {
                    panel.isFloating = false
                    panel.show()
                    area.addDockWidget(panel, Dock.KDDockWidgets.Location_OnLeft)
                }
            }
            showPanel(name)
        } finally {
            endLayoutChange()
        }
    }
    function restoreFocusedLayout() {
        beginLayoutChange()
        try {
            cancelPendingFocus()
            if (focusedPanel.length === 0) return
            focusedPanel = ""
            if (!saver.restoreFromFile(controller.dockLayoutPath)) resetLayout()
        } finally {
            endLayoutChange()
        }
    }
    Dock.DockingArea {
        id: area
        objectName: "dockArea"
        anchors.fill: parent
        uniqueName: "neverd.workbench.v1"
        Dock.DockWidget {
            id: functionsDock
            objectName: "functionsDock"
            uniqueName: "neverd.functions"
            title: qsTranslate("FunctionsPane", "FUNCTIONS")
            FunctionsPane {
                id: functions
                anchors.fill: parent
                controller: root.controller
                showTitle: false
                canRename: root.registry.activePane !== null && root.registry.activePane.selectedFunctionAddress.length > 0
                canComment: root.registry.activePane !== null && root.registry.activePane.commentReady
                property var kddockwidgets_min_size: Qt.size(200, 200)
                onRenameRequested: root.renameRequested()
                onCommentRequested: root.commentRequested()
                onNavigationRequested: address => root.navigateTo(address, false)
            }
        }
        Dock.DockWidget {
            id: machineDock
            objectName: "machineDock"
            uniqueName: "neverd.machine"
            title: (root.machineController.centralView === "cfg" ? qsTranslate("Main", "Control Flow") : root.machineController.centralView === "hex" ? qsTranslate("Main", "Hex") : qsTranslate("Main", "Disassembly")) + (root.machineController.selectedFunctionName ? " — " + root.machineController.selectedFunctionName : "")
            onIsOpenChanged: root.schedulePaneSync()
            MachinePane { id: machine; anchors.fill: parent; controller: root.machineController; codePointSize: root.codePointSize }
        }
        Dock.DockWidget {
            id: representationDock
            objectName: "representationDock"
            uniqueName: "neverd.representation"
            title: root.representationController.selectedFunctionName || qsTranslate("Main", "Pseudocode & intermediate representations")
            onIsOpenChanged: root.schedulePaneSync()
            RepresentationPane { id: representation; anchors.fill: parent; controller: root.representationController; codePointSize: root.codePointSize }
        }
        Dock.DockWidget {
            id: referencesDock
            objectName: "referencesDock"
            uniqueName: "neverd.references"
            title: qsTranslate("Main", "References")
            ReferencesPane { id: references; anchors.fill: parent; controller: root.controller }
        }
        Dock.DockWidget {
            id: outputDock
            objectName: "outputDock"
            uniqueName: "neverd.output"
            title: qsTranslate("Main", "Output")
            TextPane { id: output; anchors.fill: parent; text: root.controller.logText; emptyTitle: ""; emptyDetail: qsTranslate("Main", "Worker events and analysis messages appear here."); codePointSize: Theme.bodySize; property var kddockwidgets_min_size: Qt.size(280, 100) }
        }
        Dock.DockWidget {
            id: connectionsDock
            objectName: "connectionsDock"
            uniqueName: "neverd.connections"
            title: qsTranslate("Main", "Connections")
            ConnectionsPane { id: connections; anchors.fill: parent; client: root.client; broker: root.broker; controller: root.controller; dialogParent: root.Overlay.overlay; onDialogRequested: (dialog, focusItem) => root.connectionDialogRequested(dialog, focusItem); property var kddockwidgets_min_size: Qt.size(420, 140) }
        }
        Dock.DockWidget {
            id: extensionsDock
            objectName: "extensionsDock"
            uniqueName: "neverd.extensions"
            title: qsTranslate("Main", "Extensions")
            ExtensionsPane { id: extensions; anchors.fill: parent; controller: root.controller; onImportRequested: root.importExtensionsRequested() }
        }
    }
    NativeLayoutSaver { id: saver; objectName: "dockLayoutSaver" }
    Timer { interval: 2000; repeat: true; running: root.initialized; onTriggered: root.saveLayout() }
    Component.onCompleted: Qt.callLater(() => {
        beginLayoutChange()
        try {
            lastAnalysisPaneId = registry.activePane === representationController ? representationController.id : machineController.id
            resetLayout()
            // Establish defaults before restoration, but do not replace a
            // valid all-closed layout just because no guest is visible.
            saver.restoreFromFile(controller.dockLayoutPath)
            controller.clampWindows()
            initialized = true
        } finally {
            endLayoutChange()
        }
    })
    Connections {
        target: Qt.application
        function onScreensChanged() { root.controller.clampWindows() }
    }
}
