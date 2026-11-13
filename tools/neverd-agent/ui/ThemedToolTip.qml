import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// Shared tooltip look: small text on the menu card, rounded hairline border.
ToolTip {
    id: tip
    delay: 600
    padding: 6
    leftPadding: 8
    rightPadding: 8
    font.pointSize: Theme.smallPointSize
    contentItem: Text { text: tip.text; font: tip.font; color: Theme.text; wrapMode: Text.Wrap }
    background: Rectangle { radius: 6; color: Theme.menu; border.width: 1; border.color: Theme.borderStrong }
}
