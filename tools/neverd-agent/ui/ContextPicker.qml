import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Dialog {
    id: picker
    required property var backend
    property bool side: false
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(560, parent.width - 48)
    modal: true; padding: 18
    Overlay.modal: ModalDim {}
    background: Surface { glass: true }
    header: Label { text: qsTr("Add analysis context"); color: Theme.text; padding: 18; font.pointSize: Theme.bodyPointSize; font.weight: Font.Medium }
    contentItem: ColumnLayout {
        spacing: 12
        Label { text: qsTr("Reference an object, function or address in the current project. You can review and remove references before sending."); color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
        ChoiceField { id: objects; Layout.fillWidth: true; model: picker.backend.activeProject.objects || []; textRole: "name"; valueRole: "id" }
        Field { id: address; Layout.fillWidth: true; placeholderText: qsTr("Function name or address (leave empty for the whole object)") }
        ActionButton {
            text: qsTr("Add object or function reference"); primary: true
            enabled: !!objects.currentValue
            onClicked: { picker.backend.attachObjectReference(objects.currentValue, address.text, picker.side); picker.close() }
        }
        ActionButton {
            visible: !!picker.backend.evidence.id
            text: qsTr("Attach the evidence being viewed")
            onClicked: { picker.backend.attachEvidence(picker.backend.evidence.id, picker.side); picker.close() }
        }
        ActionButton {
            visible: picker.backend.previewTitle.length > 0
            text: qsTr("Attach the current file: %1").arg(picker.backend.previewTitle)
            onClicked: { picker.backend.attachPreview(picker.side); picker.close() }
        }
        Label { text: qsTr("You can also select text in the conversation or in pseudocode, then right-click to attach it as a reference or start a side chat."); color: Theme.quiet; wrapMode: Text.Wrap; Layout.fillWidth: true; font.pointSize: Theme.smallPointSize }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ActionButton { text: qsTr("Close"); onClicked: picker.close() }
        }
    }
}
