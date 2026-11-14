import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

// The workbench's three button faces: a command with a glyph is a tool
// button (flat until hovered, the selection color while checked), a text-only
// command is a push button in the button gray, and `primary` marks the one
// main action of a window in the accent.
ToolButton {
    id: control
    property string glyph: ""
    property string hint: text
    property bool primary: false
    property int elideMode: Text.ElideRight
    readonly property bool push: primary || (glyph.length === 0 && text.length > 0)
    readonly property color foreground: primary ? (enabled ? Theme.primaryText : Theme.primaryDisabledText)
                                      : !enabled ? Theme.disabledText
                                      : checked ? Theme.toolCheckedText
                                      : push ? Theme.buttonText : Theme.text
    implicitHeight: Theme.controlHeight
    readonly property bool labelled: text.length > 0
    implicitWidth: Math.max(push && labelled ? 75 : 34, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: push && labelled ? 14 : labelled ? 10 : 7
    rightPadding: push && labelled ? 14 : labelled ? 10 : 7
    topPadding: 4
    bottomPadding: 4
    focusPolicy: Qt.StrongFocus
    Accessible.name: hint
    ThemedToolTip { visible: control.hovered && control.hint.length > 0 && control.hint !== control.text; text: control.hint }
    contentItem: RowLayout {
        spacing: 8
        Icon {
            visible: control.glyph.length > 0
            name: control.glyph
            color: control.primary || control.checked || !control.enabled ? control.foreground
                 : control.hovered ? Theme.text : Theme.secondary
            Layout.preferredWidth: Theme.iconSize
            Layout.preferredHeight: Theme.iconSize
            Layout.alignment: Qt.AlignVCenter
        }
        Text {
            visible: control.text.length > 0
            text: control.text
            textFormat: Text.PlainText
            color: control.foreground
            font: control.font
            Layout.fillWidth: true
            elide: control.elideMode
            horizontalAlignment: control.glyph.length > 0 ? Text.AlignLeft : Text.AlignHCenter
        }
    }
    background: Rectangle {
        radius: control.push ? Theme.controlRadius : Theme.toolRadius
        color: control.primary ? (!control.enabled ? Theme.primaryDisabled
                                  : control.hovered || control.down ? Theme.primaryHover : Theme.primary)
             : control.push ? (!control.enabled ? Theme.controlDisabled
                               : control.hovered || control.down ? Theme.buttonHover : Theme.button)
             : control.checked ? Theme.toolChecked
             : control.down || control.hovered && control.enabled ? Theme.toolHover : "transparent"
        border.width: control.visualFocus ? 1 : 0
        border.color: Theme.focusBorder
    }
}
