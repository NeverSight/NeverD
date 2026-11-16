import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// A field: the input fill inside its outline, the focus color's outline while
// it has focus.
TextField {
    id: control
    color: enabled ? Theme.text : Theme.disabledText
    placeholderTextColor: Theme.quiet
    selectByMouse: true
    padding: 4
    leftPadding: 10
    rightPadding: 10
    implicitHeight: Theme.controlHeight
    selectionColor: Theme.selection
    selectedTextColor: Theme.selectionText
    ContextMenu.menu: EditMenu { editor: control }
    background: Rectangle {
        radius: Theme.controlRadius
        color: control.enabled ? Theme.raised : Theme.controlDisabled
        border.width: 1
        border.color: control.activeFocus ? Theme.focusBorder : Theme.inputBorder
    }
}
