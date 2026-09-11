pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import "qrc:/kddockwidgets/qtquick/views/qml/" as DockBase

DockBase.TitleBarBase {
    id: root
    color: Theme.sidebar
    heightWhenVisible: Theme.dockTitleHeight
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: root.isFocused ? Theme.accent : Theme.border }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 3
        spacing: 1
        Text { textFormat: Text.PlainText; text: root.title; color: root.isFocused ? Theme.foreground : Theme.muted; font.pointSize: Theme.bodySize; elide: Text.ElideRight; Layout.fillWidth: true }
        WorkbenchButton { text: "↗"; implicitHeight: Theme.compactControlHeight; implicitWidth: 28; visible: root.floatButtonVisible; hint: qsTr("Float or Dock Panel"); onClicked: root.floatButtonClicked() }
        WorkbenchButton { text: "×"; implicitHeight: Theme.compactControlHeight; implicitWidth: 28; enabled: root.closeButtonEnabled; hint: qsTr("Close Panel"); onClicked: root.closeButtonClicked() }
    }
}
