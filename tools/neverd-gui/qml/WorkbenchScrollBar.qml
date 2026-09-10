import QtQuick
import QtQuick.Controls.Basic

ScrollBar {
    // Keep scrollbar contrast independent of dialog and button borders.
    palette.mid: Theme.subdued
    palette.dark: Theme.muted
}
