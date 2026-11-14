import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

MenuItem {
    id: item
    // Hidden items collapse instead of leaving an empty row.
    implicitHeight: visible ? Math.max(24, contentItem.implicitHeight + 8) : 0
    leftPadding: 8
    rightPadding: 24
    topPadding: 4
    bottomPadding: 4
    readonly property bool lit: item.highlighted && item.enabled
    contentItem: Text {
        text: item.text
        font: item.font
        color: !item.enabled ? Theme.disabledText : item.lit ? Theme.menuSelectionText : Theme.menuText
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: null
    arrow: null
    background: Rectangle {
        radius: Theme.toolRadius
        color: item.lit || item.down ? Theme.menuSelection : "transparent"
    }
}
