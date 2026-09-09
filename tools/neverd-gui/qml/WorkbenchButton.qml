pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control
    property bool primary: false
    property string hint: ""
    implicitHeight: 30
    implicitWidth: Math.max(30, contentItem.implicitWidth + 20)
    padding: 7
    font.pointSize: Theme.bodySize
    hoverEnabled: true
    Accessible.name: text
    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.subdued : control.primary ? Theme.accentForeground : Theme.foreground
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: control.down ? Theme.selection : control.primary ? Theme.accent : control.hovered ? Theme.hover : "transparent"
        border.color: control.activeFocus ? Theme.focus : "transparent"
        border.width: 1
        radius: 2
    }
    ToolTip.visible: hint.length > 0 && (hovered || activeFocus)
    ToolTip.text: hint
    ToolTip.delay: 700
}
