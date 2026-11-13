import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// Thin overlay scrollbar: invisible track, handle appears while scrolling or hovered.
ScrollBar {
    id: bar
    padding: 2
    minimumSize: 0.08
    contentItem: Rectangle {
        implicitWidth: bar.hovered || bar.pressed ? 6 : 4
        implicitHeight: implicitWidth
        radius: width / 2
        color: bar.pressed ? Theme.secondary : bar.hovered ? Theme.quiet : Theme.borderStrong
        opacity: bar.policy === ScrollBar.AlwaysOn || (bar.active && bar.size < 1) ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Behavior on implicitWidth { NumberAnimation { duration: 90 } }
    }
    background: null
}
