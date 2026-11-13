import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

TextField {
    id: control
    color: Theme.text
    placeholderTextColor: Theme.secondary
    selectByMouse: true
    padding: 8
    leftPadding: 10
    rightPadding: 10
    implicitHeight: 32
    selectionColor: Theme.accent
    selectedTextColor: Theme.background
    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.raised
        border.width: 1
        border.color: control.activeFocus ? Theme.focusBorder : Theme.border
        Behavior on border.color { ColorAnimation { duration: 140 } }
    }
}
