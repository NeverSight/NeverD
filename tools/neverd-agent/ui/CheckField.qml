import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// Compact checkbox: 16px box, small text, accent fill when checked.
CheckBox {
    id: control
    spacing: 8
    padding: 2
    font.pointSize: Theme.smallPointSize
    indicator: Rectangle {
        x: control.leftPadding
        y: Math.round((control.height - height) / 2)
        width: 16; height: 16; radius: 4
        color: control.checked ? Theme.primary : "transparent"
        border.width: control.checked ? 0 : 1
        border.color: control.activeFocus ? Theme.focusBorder : control.hovered ? Theme.quiet : Theme.borderStrong
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
        color: Theme.text
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }
}
