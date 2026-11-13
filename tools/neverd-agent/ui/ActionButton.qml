import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ToolButton {
    id: control
    property string glyph: ""
    property string hint: text
    property bool primary: false
    property int elideMode: Text.ElideRight
    implicitHeight: Theme.controlHeight
    implicitWidth: Math.max(34, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: text.length ? 10 : 7
    rightPadding: text.length ? 10 : 7
    topPadding: 6
    bottomPadding: 6
    focusPolicy: Qt.StrongFocus
    Accessible.name: hint
    ThemedToolTip { visible: control.hovered && control.hint.length > 0; text: control.hint }
    contentItem: RowLayout {
        spacing: 8
        Icon {
            visible: control.glyph.length > 0
            name: control.glyph
            color: control.primary ? (control.enabled ? Theme.primaryText : Theme.quiet) : control.checked ? Theme.accent : control.hovered ? Theme.text : Theme.secondary
            Layout.preferredWidth: Theme.iconSize
            Layout.preferredHeight: Theme.iconSize
            Layout.alignment: Qt.AlignVCenter
            opacity: control.enabled ? 1 : 0.4
        }
        Text {
            visible: control.text.length > 0
            text: control.text
            textFormat: Text.PlainText
            color: control.primary ? (control.enabled ? Theme.primaryText : Theme.quiet) : Theme.text
            font: control.font
            opacity: control.enabled ? 1 : 0.4
            Layout.fillWidth: true
            elide: control.elideMode
            horizontalAlignment: control.glyph.length > 0 ? Text.AlignLeft : Text.AlignHCenter
        }
    }
    background: Rectangle {
        radius: Theme.controlRadius
        color: control.primary && !control.enabled ? Theme.raised
             : control.primary ? (control.down ? Qt.darker(Theme.primary, 1.15) : control.hovered ? Qt.lighter(Theme.primary, 1.1) : Theme.primary)
             : control.down ? Theme.selected
             : control.checked ? Theme.selected
             : control.hovered ? Theme.hover : "transparent"
        Behavior on color { ColorAnimation { duration: 110 } }
        opacity: control.enabled || control.primary ? 1 : 0.45
        border.width: control.visualFocus ? 1 : 0
        border.color: Theme.focusBorder
    }
}
