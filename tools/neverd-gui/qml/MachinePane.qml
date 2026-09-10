pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    objectName: "machinePane"
    required property var controller
    property real codePointSize: Theme.codeSize
    readonly property int currentIndex: ["disasm", "cfg", "hex"].indexOf(controller.centralView)
    property var kddockwidgets_min_size: Qt.size(280, 180)
    color: Theme.editor
    function focusContent() { [disassembly, graph, hex][currentIndex].focusContent() }
    function selectView(index) {
        if (index < 0 || index > 2) return
        controller.requestView(["disasm", "cfg", "hex"][index])
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        PanelTabs {
            Layout.fillWidth: true
            labels: [qsTranslate("Main", "Disassembly"), qsTranslate("Main", "CFG"), qsTranslate("Main", "Hex")]
            currentIndex: root.currentIndex
            onSelected: index => root.selectView(index)
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.currentIndex
            DisassemblyPane { id: disassembly; controller: root.controller; codePointSize: root.codePointSize }
            GraphPane { id: graph; controller: root.controller; codePointSize: root.codePointSize }
            TextPane { id: hex; text: root.controller.hexText; emptyTitle: qsTranslate("Main", "Hex view"); emptyDetail: qsTranslate("Main", "Select an address to inspect its bytes."); codePointSize: root.codePointSize }
        }
    }
}
