import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// The workbench's menu: the menu surface inside a hairline, inset 4.
Menu {
    id: menu
    padding: 4
    margins: 8
    width: 220
    delegate: ThemedMenuItem {}
    background: Rectangle {
        implicitWidth: menu.width
        radius: Theme.toolRadius
        color: Theme.menu
        border.width: 1
        border.color: Theme.borderStrong
    }
}
