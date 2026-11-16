import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// A tool tip: the menu surface inside a hairline. A long one, such as a path
// with the reason it did not open, wraps instead of leaving the window.
ToolTip {
    id: tip
    delay: 600
    padding: 5
    leftPadding: 9
    rightPadding: 9
    contentWidth: Math.min(Math.ceil(label.implicitWidth), 460)
    contentItem: Text { id: label; textFormat: Text.PlainText; text: tip.text; font: tip.font; color: Theme.menuText; wrapMode: Text.Wrap }
    background: Rectangle { radius: Theme.toolRadius; color: Theme.menu; border.width: 1; border.color: Theme.borderStrong }
}
