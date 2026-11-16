import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// What a right click or the menu key offers in a text input: the editing
// commands, in the Agent's own menu and words. From Qt 6.9 Qt Quick Controls
// gives every text input a menu of its own, in the style's look and with
// Qt's words; an input names this one as its ContextMenu.menu instead.
ThemedMenu {
    id: menu
    required property Item editor
    readonly property bool writable: !editor.readOnly
    // A field that hides what is typed gives none of it to the clipboard.
    readonly property bool shown: editor.echoMode === undefined || editor.echoMode === TextInput.Normal
    readonly property bool selection: editor.selectedText.length > 0
    ThemedMenuItem { text: qsTr("Undo"); visible: menu.writable; enabled: menu.editor.canUndo; onTriggered: menu.editor.undo() }
    ThemedMenuItem { text: qsTr("Redo"); visible: menu.writable; enabled: menu.editor.canRedo; onTriggered: menu.editor.redo() }
    ThemedSeparator { visible: menu.writable }
    ThemedMenuItem { text: qsTr("Cut"); visible: menu.writable; enabled: menu.selection && menu.shown; onTriggered: menu.editor.cut() }
    ThemedMenuItem { text: qsTr("Copy"); enabled: menu.selection && menu.shown; onTriggered: menu.editor.copy() }
    ThemedMenuItem { text: qsTr("Paste"); visible: menu.writable; enabled: menu.editor.canPaste; onTriggered: menu.editor.paste() }
    ThemedSeparator {}
    ThemedMenuItem { text: qsTr("Select all"); enabled: menu.editor.length > 0; onTriggered: menu.editor.selectAll() }
}
