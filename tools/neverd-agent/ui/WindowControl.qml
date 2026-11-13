import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

ToolButton {
    id: control
    // The workbench's caption metrics and glyph shapes, painted in QML.
    required property string kind
    required property var frame
    required property var appWindow
    readonly property bool lit: hovered || down
    readonly property color ink: kind === "close" && lit ? Theme.closeText
                                : appWindow.active ? Theme.titleText : Theme.titleInactiveText
    implicitWidth: frame.buttonWidth
    implicitHeight: frame.titleHeight
    padding: 0
    text: kind === "close" ? qsTr("关闭") : kind === "minimize" ? qsTr("最小化")
          : frame.maximized ? qsTr("还原") : qsTr("最大化")
    Accessible.name: text
    ThemedToolTip { visible: control.hovered; text: control.text }
    onClicked: {
        if (kind === "close") appWindow.close()
        else if (kind === "minimize") frame.minimize()
        else frame.toggleMaximized()
    }
    background: Rectangle {
        color: control.down ? (control.kind === "close" ? Theme.closePressed : Theme.controlPressed)
             : control.hovered ? (control.kind === "close" ? Theme.closeHover : Theme.controlHover)
             : "transparent"
        border.width: control.visualFocus ? 1 : 0
        border.color: Theme.focus
    }
    contentItem: Item {
        Item {
            anchors.centerIn: parent
            width: 10
            height: 10
            Rectangle { visible: control.kind === "minimize"; x: 0; y: 5; width: 10; height: 1; color: control.ink }
            Rectangle {
                visible: control.kind === "maximize" && !control.frame.maximized
                width: 10; height: 10; color: "transparent"; border.width: 1; border.color: control.ink
            }
            Item {
                visible: control.kind === "maximize" && control.frame.maximized
                Rectangle { x: 2; width: 8; height: 8; color: "transparent"; border.width: 1; border.color: control.ink }
                Rectangle {
                    y: 2; width: 8; height: 8
                    color: control.lit ? (control.down ? Theme.controlPressed : Theme.controlHover) : Theme.titleBar
                    border.width: 1; border.color: control.ink
                }
            }
            Item {
                visible: control.kind === "close"
                anchors.fill: parent
                Rectangle { anchors.centerIn: parent; width: 14; height: 1; rotation: 45; color: control.ink; antialiasing: true }
                Rectangle { anchors.centerIn: parent; width: 14; height: 1; rotation: -45; color: control.ink; antialiasing: true }
            }
        }
    }
}
