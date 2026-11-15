import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// The workbench's tool tip: the menu surface inside a hairline.
ToolTip {
    id: tip
    delay: 600
    padding: 4
    leftPadding: 8
    rightPadding: 8
    contentItem: Text { text: tip.text; font: tip.font; color: Theme.menuText; wrapMode: Text.Wrap }
    background: Rectangle { radius: Theme.toolRadius; color: Theme.menu; border.width: 1; border.color: Theme.borderStrong }
}
