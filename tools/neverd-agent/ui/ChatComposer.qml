import QtQuick
import QtQuick.Controls.Basic

TextArea {
    readonly property bool canSubmit: enabled && text.trim().length > 0
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
