pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    objectName: "machinePane"
    required property var controller
    property real codePointSize: Theme.codeSize
    property int currentIndex: 0
    property var kddockwidgets_min_size: Qt.size(280, 180)
    color: Theme.editor
    function selectView(index) {
        currentIndex = index
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
            DisassemblyPane { controller: root.controller; codePointSize: root.codePointSize }
            GraphPane { controller: root.controller; codePointSize: root.codePointSize }
            TextPane { text: root.controller.hexText; emptyTitle: qsTranslate("Main", "Hex view"); emptyDetail: qsTranslate("Main", "Select an address to inspect its bytes."); codePointSize: root.codePointSize }
        }
    }
}
