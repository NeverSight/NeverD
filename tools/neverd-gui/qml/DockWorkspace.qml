pragma ComponentBehavior: Bound
import QtQuick
import com.kdab.dockwidgets 2.0 as Dock

Rectangle {
    id: root
    objectName: "dockWorkspace"
    required property var controller
    required property var client
    required property var broker
    property real codePointSize: Theme.codeSize
    property bool initialized: false
    signal renameRequested()
    signal commentRequested()
    signal importExtensionsRequested()
    color: Theme.editor
    readonly property var panels: [functionsDock, machineDock, representationDock, referencesDock, outputDock, connectionsDock, extensionsDock]
    function resetLayout() {
        for (const panel of panels) {
            panel.isFloating = false
            panel.forceClose()
        }
        area.addDockWidget(functionsDock, Dock.KDDockWidgets.Location_OnLeft, null, Qt.size(250, height - 180))
        area.addDockWidget(machineDock, Dock.KDDockWidgets.Location_OnRight, functionsDock, Qt.size(Math.max(360, width - 255), height - 180))
        area.addDockWidget(representationDock, width < 1150 ? Dock.KDDockWidgets.Location_OnBottom : Dock.KDDockWidgets.Location_OnRight, machineDock, Qt.size(Math.max(360, (width - 255) / 2), Math.max(230, height / 2)))
        area.addDockWidget(referencesDock, Dock.KDDockWidgets.Location_OnBottom, null, Qt.size(width, 180))
        referencesDock.addDockWidgetAsTab(outputDock)
        referencesDock.addDockWidgetAsTab(connectionsDock)
        referencesDock.addDockWidgetAsTab(extensionsDock, Dock.KDDockWidgets.StartHidden)
        referencesDock.setAsCurrentTab()
    }
    function saveLayout() {
        // KDDW may close dock windows before ApplicationWindow.closing runs.
        // Preserve the last usable layout instead of persisting that teardown.
        if (initialized && panels.some(panel => panel.isOpen)) saver.saveToFile(controller.dockLayoutPath)
    }
    function showPanel(name) {
        for (const panel of panels) {
            if (panel.uniqueName === "neverd." + name) {
                panel.show()
                panel.raise()
                panel.setAsCurrentTab()
                return
            }
        }
    }
    function selectMachineView(index) {
        showPanel("machine")
        machine.selectView(index)
    }
    function toggleBottom() {
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
        for (const panel of panels) {
            if (panel.uniqueName !== "neverd." + name) panel.close()
            else {
                panel.isFloating = false
                panel.show()
                area.addDockWidget(panel, Dock.KDDockWidgets.Location_OnLeft)
            }
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
                anchors.fill: parent
                controller: root.controller
                showTitle: false
                property var kddockwidgets_min_size: Qt.size(200, 200)
                onRenameRequested: root.renameRequested()
                onCommentRequested: root.commentRequested()
            }
        }
        Dock.DockWidget {
            id: machineDock
            objectName: "machineDock"
            uniqueName: "neverd.machine"
            title: qsTranslate("Main", "Disassembly")
            MachinePane { id: machine; anchors.fill: parent; controller: root.controller; codePointSize: root.codePointSize }
        }
        Dock.DockWidget {
            id: representationDock
            objectName: "representationDock"
            uniqueName: "neverd.representation"
            title: root.controller.representationFunctionName || qsTranslate("Main", "Pseudocode & intermediate representations")
            RepresentationPane { anchors.fill: parent; controller: root.controller; codePointSize: root.codePointSize }
        }
        Dock.DockWidget {
            id: referencesDock
            objectName: "referencesDock"
            uniqueName: "neverd.references"
            title: qsTranslate("Main", "References")
            ReferencesPane { anchors.fill: parent; controller: root.controller }
        }
        Dock.DockWidget {
            id: outputDock
            objectName: "outputDock"
            uniqueName: "neverd.output"
            title: qsTranslate("Main", "Output")
            TextPane { anchors.fill: parent; text: root.controller.logText; emptyTitle: ""; emptyDetail: qsTranslate("Main", "Worker events and analysis messages appear here."); codePointSize: Theme.bodySize; property var kddockwidgets_min_size: Qt.size(280, 100) }
        }
        Dock.DockWidget {
            id: connectionsDock
            objectName: "connectionsDock"
            uniqueName: "neverd.connections"
            title: qsTranslate("Main", "Connections")
            ConnectionsPane { anchors.fill: parent; client: root.client; broker: root.broker; controller: root.controller; property var kddockwidgets_min_size: Qt.size(420, 140) }
        }
        Dock.DockWidget {
            id: extensionsDock
            objectName: "extensionsDock"
            uniqueName: "neverd.extensions"
            title: qsTranslate("Main", "Extensions")
            ExtensionsPane { anchors.fill: parent; controller: root.controller; onImportRequested: root.importExtensionsRequested() }
        }
    }
    Dock.LayoutSaver { id: saver; objectName: "dockLayoutSaver" }
    Timer { interval: 2000; repeat: true; running: root.initialized; onTriggered: root.saveLayout() }
    Component.onCompleted: Qt.callLater(() => {
        resetLayout()
        saver.restoreFromFile(controller.dockLayoutPath)
        // A valid file may describe a workspace with every panel closed.
        // Recover a useful initial view instead of reopening an empty window.
        if (!panels.some(panel => panel.isOpen)) resetLayout()
        controller.clampWindows()
        initialized = true
    })
    Connections {
        target: Qt.application
        function onScreensChanged() { root.controller.clampWindows() }
    }
}
