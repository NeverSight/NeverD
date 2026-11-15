import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ColumnLayout {
    id: view
    required property var backend
    signal addRequested(string objectId)
    signal editRequested(string objectId, string id, string text)
    spacing: 12

    Label {
        Layout.fillWidth: true
        text: view.backend.memoryStatus || qsTr("After a task ends, memory is organized while idle and recalled for related questions later.")
        color: Theme.secondary
        wrapMode: Text.Wrap
    }
    RowLayout {
        Layout.fillWidth: true
        Label {
            text: (view.backend.useMemory ? qsTr("Recall on") : qsTr("Recall off")) + " · " +
                (view.backend.generateMemory ? qsTr("Auto-organize on") : qsTr("Auto-organize off"))
            color: Theme.secondary
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }
        ActionButton {
            glyph: "refresh"; hint: qsTr("Organize pending records now and retry failed ones")
            enabled: view.backend.generateMemory && !view.backend.memoryWorking
            onClicked: view.backend.organizeMemory()
        }
    }
    component Entry: ColumnLayout {
        id: entry
        required property var record
        required property string scopeId
        property bool expanded: false
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: (entry.record.state === "needs_review" ? qsTr("%1 · Conflict needs confirmation") :
                 entry.record.state === "superseded" ? qsTr("%1 · Superseded") :
                 "%1").arg(entry.record.title || qsTr("Manual memory"))
            color: entry.record.state === "needs_review" ? Theme.error : Theme.text
            font.weight: Font.Medium
            wrapMode: Text.Wrap
        }
        TextEdit {
            Layout.fillWidth: true
            text: entry.record.text
            textFormat: TextEdit.PlainText
            readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
            color: entry.record.state === "superseded" ? Theme.secondary : Theme.text
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: entry.record.automatic ?
                    ({preference: qsTr("User preference"), procedure: qsTr("Research method"), finding: qsTr("Past finding · Needs verification"), hypothesis: qsTr("Hypothesis")}[entry.record.category] || qsTr("Extracted automatically")) : qsTr("Added manually")
                color: Theme.secondary; Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            ActionButton {
                glyph: "comment"; hint: qsTr("Edit or confirm this version…")
                onClicked: view.editRequested(entry.scopeId, entry.record.id, entry.record.text)
            }
            ActionButton {
                glyph: "close_small"; hint: qsTr("Forget, and stop automatic organizing from adding it back")
                onClicked: view.backend.forgetMemory(entry.scopeId, entry.record.id)
            }
        }
        ActionButton {
            visible: !!entry.record.source
            text: entry.expanded ? qsTr("Hide source") : qsTr("Show source")
            onClicked: entry.expanded = !entry.expanded
        }
        ColumnLayout {
            visible: entry.expanded && !!entry.record.source
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: entry.record.source ? qsTr("Original quote · %1").arg(entry.record.source.createdAt) : ""
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            TextEdit {
                Layout.fillWidth: true
                text: entry.record.source ? entry.record.source.quote : ""
                textFormat: TextEdit.PlainText
                readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                color: Theme.secondary
            }
            Label {
                text: entry.record.source ? qsTr("Source task: %1").arg(entry.record.source.runId) : ""
                color: Theme.secondary; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true
            }
            Repeater {
                model: entry.record.source ? entry.record.source.evidence : []
                delegate: ActionButton {
                    required property var modelData
                    text: modelData.tool + (modelData.address ? " · " + modelData.address : "")
                    glyph: "file"
                    enabled: entry.record.originProjectId === view.backend.activeId
                    hint: enabled ? qsTr("Open the original evidence snapshot") : qsTr("The evidence is stored in its source project; this entry keeps the reference and original quote.")
                    onClicked: view.backend.openEvidence(modelData.id)
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
    }
    ScrollView {
        Layout.fillWidth: true; Layout.fillHeight: true
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            width: parent.width
            spacing: 14
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Project memory"); color: Theme.secondary; Layout.fillWidth: true }
                ActionButton { glyph: "plus"; hint: qsTr("Add project memory…"); onClicked: view.addRequested("") }
            }
            Label {
                visible: view.backend.projectMemory.length === 0
                text: qsTr("Project preferences, research methods and leads across objects are kept here.")
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Repeater {
                model: view.backend.projectMemory
                delegate: Entry { required property var modelData; record: modelData; scopeId: "" }
            }
            Label {
                visible: !!view.backend.selectedObjectId
                text: qsTr("%1 · Object memory").arg(view.backend.selectedObject.name || "")
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Label {
                visible: !!view.backend.selectedObjectId && view.backend.memory.length === 0
                text: qsTr("No object memory yet. Extracted findings are always tied to this file's SHA-256.")
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Repeater {
                model: view.backend.memory
                delegate: Entry { required property var modelData; record: modelData; scopeId: view.backend.selectedObjectId }
            }
            Label {
                text: view.backend.sharing ? qsTr("From other projects · Read-only") : qsTr("Object memory sharing across projects is off.")
                color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Repeater {
                model: view.backend.sharedMemory
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label { text: modelData.sourceProject; color: Theme.secondary }
                    TextEdit {
                        Layout.fillWidth: true
                        text: modelData.text; textFormat: TextEdit.PlainText
                        readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; color: Theme.text
                    }
                }
            }
        }
    }
}
