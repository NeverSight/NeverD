import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// Context/drop-down menu: rounded card, hairline border, compact rows.
Menu {
    id: menu
    padding: 4
    margins: 8
    width: 220
    delegate: ThemedMenuItem {}
    background: Rectangle {
        implicitWidth: menu.width
        radius: Theme.controlRadius
        color: Theme.menu
        border.width: 1
        border.color: Theme.borderStrong
    }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 } }
}
