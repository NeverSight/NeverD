import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// The workbench's check box: a 14-pixel box 6 from its label, the main
// button's blue with a white mark while checked.
CheckBox {
    id: control
    spacing: 6
    padding: 2
    indicator: Rectangle {
        x: control.leftPadding
        y: Math.round((control.height - height) / 2)
        width: 14; height: 14; radius: Theme.toolRadius
        color: control.checked ? Theme.checkBoxChecked : Theme.checkBox
        border.width: 1
        border.color: control.visualFocus ? Theme.focusBorder
                    : control.checked ? Theme.checkBoxChecked : Theme.checkBoxBorder
        opacity: control.enabled ? 1 : 0.4
        Icon {
            anchors.centerIn: parent
            width: 12; height: 12
            visible: control.checked
            name: "check"
            color: Theme.primaryText
        }
    }
    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.disabledText
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }
}
