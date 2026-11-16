import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// A check box: a 16-pixel box 8 from its label, the main button's blue with
// a white mark while checked. The box leads the label: at the right in a
// right-to-left layout.
CheckBox {
    id: control
    spacing: 8
    padding: 2
    indicator: Rectangle {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: Math.round((control.height - height) / 2)
        width: 16; height: 16; radius: Theme.checkRadius
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
        textFormat: Text.PlainText
        leftPadding: control.mirrored ? 0 : control.indicator.width + control.spacing
        rightPadding: control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.disabledText
        horizontalAlignment: Text.AlignLeft
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }
}
