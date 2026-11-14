import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// A workbench field: the input fill inside its outline, the focus color's
// outline while it has focus.
TextField {
    id: control
    color: enabled ? Theme.text : Theme.disabledText
    placeholderTextColor: Theme.quiet
    selectByMouse: true
    padding: 4
    leftPadding: 6
    rightPadding: 6
    implicitHeight: Theme.controlHeight
    selectionColor: Theme.selection
    selectedTextColor: Theme.selectionText
    background: Rectangle {
        radius: Theme.controlRadius
        color: control.enabled ? Theme.raised : Theme.controlDisabled
        border.width: 1
        border.color: control.activeFocus ? Theme.focusBorder : Theme.inputBorder
    }
}
