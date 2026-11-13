import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Flow {
    id: chips
    property var references: []
    signal removeRequested(int index)
    spacing: 6
    visible: references.length > 0
    Repeater {
        model: chips.references
        Row {
            required property var modelData
            required property int index
            spacing: 2
            ActionButton {
                text: modelData.title; implicitHeight: 27
                width: Math.min(230, implicitWidth)
                hint: qsTr("查看将要发送的引用")
                onClicked: referenceDetails.open()
                Popup {
                    id: referenceDetails
                    parent: Overlay.overlay
                    anchors.centerIn: parent
                    width: Math.min(600, parent.width - 40); height: Math.min(400, parent.height - 40)
                    padding: 18; modal: true
                    background: Surface { glass: true }
                    contentItem: ScrollView {
                        clip: true
                        TextArea { text: JSON.stringify(modelData, null, 2); readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; color: Theme.text; background: null }
                    }
                }
            }
            ActionButton { glyph: "close_small"; implicitHeight: 27; hint: qsTr("移除引用"); onClicked: chips.removeRequested(index) }
        }
    }
}
