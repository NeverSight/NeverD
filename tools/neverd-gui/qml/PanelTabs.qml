pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    property var labels: []
    property int currentIndex: 0
    property string trailingText: ""
    signal selected(int index)
    implicitHeight: Theme.tabHeight
    color: Theme.sidebar
    RowLayout {
        anchors.fill: parent
        spacing: 0
        Repeater {
            model: root.labels
            delegate: Button {
                id: tab
                required property string modelData
                required property int index
                Layout.fillHeight: true
                implicitWidth: label.implicitWidth + 24
                hoverEnabled: true
                Accessible.name: modelData
                Accessible.role: Accessible.PageTab
                Accessible.selected: root.currentIndex === index
                contentItem: Text {
                    id: label
                    textFormat: Text.PlainText
                    text: tab.modelData
                    color: root.currentIndex === tab.index ? Theme.foreground : Theme.muted
                    font.pointSize: Theme.bodySize
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    color: root.currentIndex === tab.index ? Theme.editor : tab.hovered ? Theme.hover : Theme.elevated
                    border.color: tab.visualFocus ? Theme.focus : "transparent"
                    Rectangle {
                        width: parent.width
                        height: 2
                        color: Theme.accent
                        visible: root.currentIndex === tab.index
                    }
                }
                onClicked: root.selected(index)
            }
        }
        Item { Layout.fillWidth: true }
        Text {
            text: root.trailingText
            visible: text.length > 0
            color: Theme.subdued
            font.pointSize: Theme.captionSize
            Layout.rightMargin: 12
            elide: Text.ElideRight
        }
    }
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
}
