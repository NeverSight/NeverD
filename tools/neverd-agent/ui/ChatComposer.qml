import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

TextArea {
    id: composer
    readonly property bool canSubmit: enabled && text.trim().length > 0
    ContextMenu.menu: EditMenu { editor: composer }
    signal submitRequested()

    Keys.onPressed: event => {
        if (inputMethodComposing || (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)
                || (event.modifiers & ~Qt.KeypadModifier) !== Qt.NoModifier)
            return
        event.accepted = true
        if (!event.isAutoRepeat && canSubmit)
            submitRequested()
    }
}
