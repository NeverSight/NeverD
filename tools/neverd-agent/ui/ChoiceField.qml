import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

ComboBox {
    id: control
    implicitHeight: Theme.controlHeight
    implicitWidth: 160
    leftPadding: 10
    rightPadding: 28
    contentItem: Text {
        text: control.displayText
        textFormat: Text.PlainText
        color: control.enabled ? Theme.text : Theme.disabledText
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.controlRadius
        color: control.enabled ? Theme.raised : Theme.controlDisabled
        border.width: 1
        border.color: control.activeFocus || control.popup.visible ? Theme.focusBorder : Theme.inputBorder
    }
    indicator: Icon {
        name: "chevron_down"
        x: control.width - width - 8
        y: Math.round((control.height - height) / 2)
        width: 16
        height: 16
        color: Theme.quiet
    }
    delegate: ItemDelegate {
        required property var modelData
        required property int index
        width: control.width - 8
        implicitHeight: 26
        text: control.textRole ? modelData[control.textRole] : modelData
        highlighted: control.highlightedIndex === index
        contentItem: Text { text: parent.text; textFormat: Text.PlainText; color: parent.highlighted || parent.hovered ? Theme.menuSelectionText : Theme.menuText; font: control.font; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { radius: Theme.toolRadius; color: parent.highlighted || parent.hovered ? Theme.menuSelection : "transparent" }
    }
    popup: Popup {
        y: control.height + 4
        width: control.width
        padding: 4
        implicitHeight: contentItem.implicitHeight + 8
        background: Surface { glass: true; radius: Theme.toolRadius }
        contentItem: ListView {
            clip: true
            implicitHeight: Math.min(contentHeight, 280)
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
}
