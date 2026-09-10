pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    objectName: "representationPane"
    required property var controller
    property real codePointSize: Theme.codeSize
    property var kddockwidgets_min_size: Qt.size(300, 180)
    readonly property var representationIds: ["c", "low", "med", "high", "llvm"]
    color: Theme.editor
    function focusContent() { code.focusContent() }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        PanelTabs {
            Layout.fillWidth: true
            labels: ["C", "LowIR", "MedIR", "HighIR", "LLVM IR"]
            currentIndex: Math.max(0, root.representationIds.indexOf(root.controller.representation))
            onSelected: index => root.controller.setRepresentation(root.representationIds[index])
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            color: Theme.editor
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 6
                Text { textFormat: Text.PlainText; text: root.controller.representationFunctionName || qsTranslate("Main", "Pseudocode & intermediate representations"); color: root.controller.representationFunctionName ? Theme.functionName : Theme.subdued; font.pointSize: Theme.captionSize; elide: Text.ElideRight; Layout.fillWidth: true; LayoutMirroring.enabled: false }
                WorkbenchButton { text: qsTr("Refresh"); enabled: root.controller.loaded && !root.controller.busy; implicitHeight: 25; onClicked: root.controller.reloadRepresentation() }
                WorkbenchButton { text: root.controller.representationPinned ? qsTr("Unpin") : qsTr("Pin"); hint: qsTr("Keep this function while navigating"); checked: root.controller.representationPinned; enabled: root.controller.loaded; implicitHeight: Theme.compactControlHeight; onClicked: root.controller.toggleRepresentationPin() }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        TextPane {
            id: code
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: root.controller.representationText
            syntaxHighlight: true
            mappings: root.controller.textMappings
            selectedAddress: root.controller.selectedAddress
            onSourceLineSelected: line => root.controller.selectTextLine(line)
            codePointSize: root.codePointSize
            emptyTitle: root.controller.loaded ? qsTranslate("Main", "No representation available") : qsTranslate("Main", "Read beyond assembly")
            emptyDetail: root.controller.loaded ? root.controller.representationStatus : qsTranslate("Main", "Compare recovered C with LowIR, MedIR, HighIR, and LLVM IR. Select a function to begin.")
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            visible: root.controller.representationStatus.length > 0 && root.controller.representationText.length > 0
            Text { textFormat: Text.PlainText; Layout.fillWidth: true; text: root.controller.representationStatus + " · " + root.controller.mappingStatus; color: Theme.subdued; font.pointSize: Theme.captionSize; wrapMode: Text.WordWrap }
            WorkbenchButton { text: qsTr("Load more lines"); visible: root.controller.hasMoreText; enabled: !root.controller.busy; onClicked: root.controller.loadMoreText() }
        }
    }
}
