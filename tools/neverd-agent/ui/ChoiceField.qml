import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

ComboBox {
    id: control
    // `quiet` shows the choice as a tool button does: no field around it
    // until the pointer is on it, for a picker inside another surface.
    property bool quiet: false
    // The width that shows the longest choice whole, for a picker beside
    // its label: a fixed width cut a longer language's choices.
    readonly property int fittingWidth: {
        let widest = 0
        for (let i = 0; i < count; ++i)
            widest = Math.max(widest, choiceMetrics.advanceWidth(textAt(i)))
        return Math.ceil(widest) + leftPadding + rightPadding + 2
    }
    FontMetrics { id: choiceMetrics; font: control.font }
    implicitHeight: quiet ? Theme.rowHeight : Theme.controlHeight
    implicitWidth: 160
    leftPadding: mirrored ? 28 : 10
    rightPadding: mirrored ? 10 : 28
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
        color: control.quiet ? (control.down || control.popup.visible ? Theme.toolHover
                                : control.hovered ? Theme.hover : "transparent")
             : control.enabled ? Theme.raised : Theme.controlDisabled
        border.width: control.quiet && !control.visualFocus
                      && !(Theme.highContrast && (control.hovered || control.popup.visible)) ? 0 : 1
        border.color: control.quiet ? (control.visualFocus ? Theme.focusBorder : Theme.toolHoverBorder)
                    : control.activeFocus || control.popup.visible ? Theme.focusBorder : Theme.inputBorder
    }
    indicator: Icon {
        name: "chevron_down"
        x: control.mirrored ? 8 : control.width - width - 8
        y: Math.round((control.height - height) / 2)
        width: 16
        height: 16
        color: Theme.quiet
    }
    delegate: ItemDelegate {
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : control.width - 8
        implicitHeight: Theme.rowHeight - 2
        leftPadding: 8
        rightPadding: 8
        text: control.textRole ? modelData[control.textRole] : modelData
        highlighted: control.highlightedIndex === index
        contentItem: Text { text: parent.text; textFormat: Text.PlainText; color: parent.highlighted || parent.hovered ? Theme.menuSelectionText : Theme.menuText; font: control.font; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { radius: Theme.toolRadius; color: parent.highlighted || parent.hovered ? Theme.menuSelection : "transparent" }
    }
    popup: Popup {
        y: control.height + 4
        // A quiet picker is only as wide as its choice; its list is as wide
        // as a menu.
        width: Math.max(control.width, control.quiet ? 220 : 0)
        padding: 4
        implicitHeight: contentItem.implicitHeight + 8
        background: Surface { glass: true; radius: Theme.radius }
        contentItem: ListView {
            clip: true
            implicitHeight: Math.min(contentHeight, 280)
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
}
