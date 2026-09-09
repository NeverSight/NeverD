import QtQuick
import "qrc:/kddockwidgets/qtquick/views/qml/" as DockBase

DockBase.Separator {
    color: pointer.hovered ? Theme.accent : Theme.border
    HoverHandler { id: pointer }
}
