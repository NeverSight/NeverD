import QtQuick
import "qrc:/kddockwidgets/qtquick/views/qml/" as DockBase

DockBase.Separator {
    id: root
    color: pointer.hovered ? Theme.accent : Theme.editor
    Rectangle {
        anchors.centerIn: parent
        width: root.width > root.height ? root.width : 1
        height: root.width > root.height ? 1 : root.height
        color: pointer.hovered ? Theme.accent : Theme.border
    }
    HoverHandler { id: pointer }
}
