import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// A tool tip: the menu surface inside a hairline.
ToolTip {
    id: tip
    delay: 600
    padding: 5
    leftPadding: 9
    rightPadding: 9
    contentItem: Text { text: tip.text; font: tip.font; color: Theme.menuText; wrapMode: Text.Wrap }
    background: Rectangle { radius: Theme.toolRadius; color: Theme.menu; border.width: 1; border.color: Theme.borderStrong }
}
