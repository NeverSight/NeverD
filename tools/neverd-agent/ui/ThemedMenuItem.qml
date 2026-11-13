import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

MenuItem {
    id: item
    // Hidden items collapse instead of leaving an empty 30px row.
    implicitHeight: visible ? 30 : 0
    leftPadding: 10
    rightPadding: 10
    topPadding: 0
    bottomPadding: 0
    font.pointSize: Theme.smallPointSize
    contentItem: Text {
        text: item.text
        font: item.font
        color: item.enabled ? Theme.text : Theme.quiet
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: null
    arrow: null
    background: Rectangle {
        radius: 6
        color: item.down ? Theme.selected : item.highlighted && item.enabled ? Theme.hover : "transparent"
    }
}
