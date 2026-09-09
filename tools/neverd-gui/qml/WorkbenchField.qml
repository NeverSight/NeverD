pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: control
    implicitHeight: 30
    font.pointSize: Theme.bodySize
    color: Theme.foreground
    placeholderTextColor: Theme.subdued
    selectionColor: Theme.selection
    selectedTextColor: Theme.accentForeground
    leftPadding: 9
    rightPadding: 9
    selectByMouse: true
    background: Rectangle {
        color: Theme.editor
        border.color: control.activeFocus ? Theme.focus : Theme.border
        radius: 2
    }
}
