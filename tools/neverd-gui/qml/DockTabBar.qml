pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import "qrc:/kddockwidgets/qtquick/views/qml/" as DockBase

DockBase.TabBarBase {
    id: root
    implicitHeight: Theme.tabHeight
    function getTabAtIndex(index) { return tabs.itemAt(index) }
    function getTabIndexAtPosition(globalPoint) {
        for (let index = 0; index < tabs.count; ++index) {
            const item = tabs.itemAt(index)
            const position = item.mapFromGlobal(globalPoint.x, globalPoint.y)
            if (item.contains(position)) return index
        }
        return bar.currentIndex
    }
    onCurrentTabIndexChanged: bar.currentIndex = currentTabIndex
    TabBar {
        id: bar
        anchors.fill: parent
        onCurrentIndexChanged: root.currentTabIndex = currentIndex
        background: Rectangle { color: Theme.sidebar }
        Repeater {
            id: tabs
            model: root.tabBarCpp ? root.tabBarCpp.dockWidgetModel : 0
            delegate: TabButton {
                id: tab
                required property int index
                required property string title
                readonly property int tabIndex: index
                width: label.implicitWidth + 28
                height: Theme.tabHeight
                contentItem: Text { textFormat: Text.PlainText; id: label; text: tab.title; color: tab.checked ? Theme.foreground : Theme.muted; font.pointSize: Theme.bodySize; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter }
                background: Rectangle {
                    color: tab.checked ? Theme.editor : Theme.elevated
                    border.color: tab.visualFocus ? Theme.focus : "transparent"
                    Rectangle { height: 2; width: parent.width; visible: tab.checked; color: Theme.accent }
                }
                Accessible.name: title
            }
        }
    }
}
