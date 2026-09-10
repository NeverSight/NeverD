pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Native 1.0

Rectangle {
    id: root
    required property var controller
    property real zoom: 1
    property real codePointSize: Theme.codeSize
    readonly property var summary: controller.graphSummary
    readonly property var graphBounds: summary.bounds || { x: 0, y: 0, width: 1, height: 1 }
    readonly property string layoutRevision: summary.layout_revision || ""
    color: Theme.editor
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true
    function focusContent() { viewport.forceActiveFocus(Qt.ShortcutFocusReason) }

    function scheduleRefresh() { if (!refresh.running) refresh.start() }
    function requestViewport() {
        if (!visible || layoutRevision.length === 0 || viewport.width <= 0 || viewport.height <= 0) return
        controller.requestGraphViewport(graphBounds.x + viewport.contentX / zoom,
                                        graphBounds.y + viewport.contentY / zoom,
                                        viewport.width / zoom, viewport.height / zoom, zoom)
    }
    function setZoom(value) {
        const centerX = (viewport.contentX + viewport.width / 2) / zoom
        const centerY = (viewport.contentY + viewport.height / 2) / zoom
        zoom = Math.max(0.001, Math.min(4, value))
        viewport.contentX = Math.max(0, Math.min(viewport.contentWidth - viewport.width, centerX * zoom - viewport.width / 2))
        viewport.contentY = Math.max(0, Math.min(viewport.contentHeight - viewport.height, centerY * zoom - viewport.height / 2))
        root.scheduleRefresh()
    }
    function fitGraph() {
        setZoom(Math.min(viewport.width / Math.max(1, graphBounds.width), viewport.height / Math.max(1, graphBounds.height)))
        viewport.contentX = 0
        viewport.contentY = 0
    }
    onLayoutRevisionChanged: {
        zoom = 1
        viewport.contentX = 0
        viewport.contentY = 0
        root.scheduleRefresh()
    }
    onVisibleChanged: if (visible) root.scheduleRefresh()
    Timer { id: refresh; interval: 80; onTriggered: root.requestViewport() }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 4
            spacing: 2
            WorkbenchButton { text: "−"; hint: qsTr("Zoom out"); onClicked: root.setZoom(root.zoom / 1.25) }
            WorkbenchButton { text: Math.round(root.zoom * 1000) / 10 + "%"; hint: qsTr("Reset zoom"); onClicked: root.setZoom(1) }
            WorkbenchButton { text: "+"; hint: qsTr("Zoom in"); onClicked: root.setZoom(root.zoom * 1.25) }
            WorkbenchButton { text: qsTr("Fit"); hint: qsTr("Fit graph in viewport"); enabled: root.layoutRevision.length > 0; onClicked: root.fitGraph() }
            Item { Layout.fillWidth: true }
            Text { text: qsTr("%1 blocks · %2 edges").arg(root.summary.node_count || 0).arg(root.summary.edge_count || 0); color: Theme.subdued; font.pointSize: Theme.captionSize; Layout.rightMargin: 10 }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Flickable {
                id: viewport
                objectName: "graphViewport"
                anchors.fill: parent
                clip: true
                contentWidth: Math.max(width, root.graphBounds.width * root.zoom)
                contentHeight: Math.max(height, root.graphBounds.height * root.zoom)
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: WorkbenchScrollBar {}
                ScrollBar.horizontal: WorkbenchScrollBar {}
                onContentXChanged: root.scheduleRefresh()
                onContentYChanged: root.scheduleRefresh()
                onWidthChanged: root.scheduleRefresh()
                onHeightChanged: root.scheduleRefresh()
                NativeGraphItem {
                    id: graph
                    objectName: "nativeGraph"
                    x: viewport.contentX
                    y: viewport.contentY
                    width: viewport.width
                    height: viewport.height
                    nodes: root.controller.graphNodes
                    edges: root.controller.graphEdges
                    viewportX: root.graphBounds.x + viewport.contentX / root.zoom
                    viewportY: root.graphBounds.y + viewport.contentY / root.zoom
                    zoom: root.zoom
                    selectedAddress: root.controller.selectedAddress
                    codeFont: Qt.font({ family: Theme.monoFont, pointSize: root.codePointSize })
                    graphPalette: ({ body: Theme.sidebar, border: Theme.border, focus: Theme.focus,
                                  foreground: Theme.foreground, label: Theme.variable,
                                  edge: Theme.keyword, branch: Theme.controlFlow })
                    Accessible.role: Accessible.Pane
                    Accessible.name: qsTr("Control flow")
                    MouseArea {
                        anchors.fill: parent
                        onClicked: mouse => {
                            root.focusContent()
                            const address = graph.addressAt(mouse.x, mouse.y)
                            if (address.length) root.controller.selectInstruction(address)
                        }
                        onDoubleClicked: mouse => {
                            const address = graph.addressAt(mouse.x, mouse.y)
                            if (address.length) root.controller.navigate(address)
                        }
                        onWheel: wheel => {
                            if (wheel.modifiers & Qt.ControlModifier) {
                                root.setZoom(root.zoom * (wheel.angleDelta.y > 0 ? 1.15 : 1 / 1.15))
                                wheel.accepted = true
                            } else wheel.accepted = false
                        }
                    }
                }
            }
            EmptyPane {
                anchors.fill: parent
                visible: root.layoutRevision.length === 0
                title: qsTr("Control flow")
                detail: root.controller.loaded ? qsTr("Select a function to inspect its basic blocks.") : qsTr("Explore the paths through a function.")
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.margins: 8
            visible: text.length > 0
            text: root.controller.graphViewportStatus
            color: Theme.subdued
            font.pointSize: Theme.captionSize
            wrapMode: Text.WordWrap
        }
    }
}
