import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// The workbench's scroll bar slider, shown while scrolling or hovered.
ScrollBar {
    id: bar
    padding: 2
    minimumSize: 0.08
    contentItem: Rectangle {
        implicitWidth: 8
        implicitHeight: implicitWidth
        color: bar.pressed ? Theme.scrollbarActive : bar.hovered ? Theme.scrollbarHover : Theme.scrollbar
        opacity: bar.policy === ScrollBar.AlwaysOn || (bar.active && bar.size < 1) ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
    }
    background: null
}
