import QtQuick
import NeverD.Agent

// A flat card. `glass` marks floating surfaces (dialogs, popups) that sit above
// the workspace; they get the menu fill and a stronger hairline instead of blur.
Rectangle {
    property bool glass: false
    radius: Theme.radius
    color: glass ? Theme.menu : Theme.surface
    border.width: glass ? 1 : 0
    border.color: glass ? Theme.borderStrong : Theme.border
    antialiasing: true
}
