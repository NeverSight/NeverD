pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    property bool primary: false
    property string hint: ""
    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(30, contentItem.implicitWidth + 20)
    horizontalPadding: 7
    verticalPadding: 4
    font.pointSize: Theme.bodySize
    hoverEnabled: true
    Accessible.name: hint.length > 0 ? hint : text
    contentItem: Text {
        textFormat: Text.PlainText
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.subdued : control.primary ? Theme.accentForeground : Theme.foreground
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: !control.enabled ? "transparent" : control.down ? Theme.selection : control.primary ? (control.hovered ? Theme.accentHover : Theme.accent) : control.checked ? Theme.selection : control.hovered ? Theme.hover : "transparent"
        border.color: control.visualFocus ? Theme.focus : control.checked ? Theme.border : "transparent"
        border.width: 1
        radius: 2
    }
    ToolTip.visible: hint.length > 0 && (hovered || activeFocus)
    ToolTip.text: hint
    ToolTip.delay: 700
}
