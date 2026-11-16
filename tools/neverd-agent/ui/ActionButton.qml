import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

// Three button faces. A command with a glyph is a tool button: flat until
// hovered, a quiet fill while it is on, and with a label also a row of a list.
// A text-only command is a push button in the button gray, and `primary` marks
// the one main action of a window in the accent. `quiet` gives a text command
// the tool face, for secondary actions in rows and cards.
ToolButton {
    id: control
    property string glyph: ""
    property string hint: text
    property bool primary: false
    property bool quiet: false
    // A glyph in a disc, for the button that sends.
    property bool round: false
    property int elideMode: Text.ElideRight
    readonly property bool push: primary || (!quiet && glyph.length === 0 && text.length > 0)
    readonly property color foreground: primary ? (enabled ? Theme.primaryText : Theme.primaryDisabledText)
                                      : !enabled ? Theme.disabledText
                                      : checked ? Theme.rowSelectedText
                                      : push ? Theme.buttonText : Theme.text
    readonly property bool labelled: text.length > 0
    implicitHeight: push && !round ? Theme.controlHeight : Theme.rowHeight
    // A glyph alone sits in a square; a push button is never narrower than
    // a short word and its padding.
    implicitWidth: Math.max(push && labelled ? 76 : implicitHeight,
                            contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: push && labelled ? 14 : labelled ? 8 : 7
    rightPadding: push && labelled ? 14 : labelled ? 10 : 7
    topPadding: 4
    bottomPadding: 4
    focusPolicy: Qt.StrongFocus
    Accessible.name: hint
    // A click answers the hint: it stays away until the pointer comes back,
    // so it never covers a menu the button opened.
    property bool hintAnswered: false
    onHoveredChanged: if (!hovered) hintAnswered = false
    onPressed: hintAnswered = true
    ThemedToolTip { visible: control.hovered && !control.hintAnswered && control.hint.length > 0 && control.hint !== control.text; text: control.hint }
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
        radius: control.round ? height / 2 : control.push ? Theme.controlRadius : Theme.toolRadius
        color: control.primary ? (!control.enabled ? Theme.primaryDisabled
                                  : control.hovered || control.down ? Theme.primaryHover : Theme.primary)
             : control.push ? (!control.enabled ? Theme.controlDisabled
                               : control.hovered || control.down ? Theme.buttonHover : Theme.button)
             : control.checked ? Theme.rowSelected
             : !control.enabled ? "transparent"
             : control.down ? Theme.toolHover
             // A row answers the pointer as a list does, a glyph alone as a
             // tool bar's button.
             : control.hovered ? (control.labelled ? Theme.hover : Theme.toolHover) : "transparent"
        // Keyboard focus rings any face; high contrast also outlines push
        // and main faces, whose fills match the window there, and a tool
        // face under the pointer, as the workbench does.
        border.width: control.visualFocus || (Theme.highContrast && (control.push || control.checked
                      || control.hovered || control.down)) ? 1 : 0
        border.color: control.visualFocus ? Theme.focusBorder
                    : control.primary ? Theme.primaryBorder
                    : control.push ? Theme.inputBorder : Theme.toolHoverBorder
    }
}
