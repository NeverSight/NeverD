import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ColumnLayout {
    id: view
    required property var backend
    readonly property var record: backend.evidence
    readonly property string displayText: {
        const text = record.text || ""
        const end = text.indexOf("\n")
        if ((record.offset || 0) === 0 && end > 0) {
            try {
                const header = JSON.parse(text.slice(0, end))
                if (header.revision === record.revision && (header.representation || header.address))
                    return text.slice(end + 1)
            } catch (error) { /* Ordinary source has no metadata line. */ }
        }
        return text
    }
    spacing: 10
    ChoiceField {
        id: objects
        Layout.fillWidth: true; textRole: "name"; valueRole: "id"
        model: view.backend.activeProject.objects || []
        Accessible.name: qsTr("Analysis object for evidence")
    }
    Field { id: query; Layout.fillWidth: true; placeholderText: qsTr("Function name or address, e.g. 0x140001000") }
    Flow {
        Layout.fillWidth: true; spacing: 6
        Repeater {
            model: [{id:"c",title:qsTr("Pseudocode")},{id:"disasm",title:qsTr("Disassembly")},{id:"xrefs",title:qsTr("Cross references")},{id:"cfg",title:qsTr("Control flow") }]
            // A view switch: quiet tool buttons, the shown view checked.
            ActionButton {
                required property var modelData
                quiet: true
                text: modelData.title
                checked: view.record.representation === modelData.id || (!view.record.representation && view.record.tool === ({c: "read_decompilation", disasm: "read_disassembly", xrefs: "list_xrefs", cfg: "read_cfg"})[modelData.id])
                enabled: !view.backend.reviewBusy && !view.backend.evidenceBusy && !!objects.currentValue && query.text.trim().length > 0
                onClicked: view.backend.inspectObject(objects.currentValue, query.text, modelData.id)
            }
        }
    }
    Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondary; font.pointSize: Theme.smallPointSize
        text: view.backend.evidenceBusy ? qsTr("Reading and saving evidence…") : view.record.id
              ? qsTr("%1 · %2\nAnalysis revision %3 · %4").arg(view.record.objectName).arg(view.record.address || view.record.tool).arg(view.record.revision || "—").arg(new Date(view.record.createdAt).toLocaleString(Qt.locale(), Locale.ShortFormat))
              : qsTr("Read a function, or click an evidence link in an answer.")
    }
    Label { visible: !!view.record.issue; text: view.record.issue || ""; color: Theme.error; wrapMode: Text.Wrap; Layout.fillWidth: true }
    RowLayout {
        visible: !!view.record.id; Layout.fillWidth: true
        ActionButton { quiet: true; glyph: "plus"; text: qsTr("Attach as reference"); onClicked: view.backend.attachEvidence(view.record.id) }
        ActionButton { quiet: true; glyph: "float"; text: qsTr("Open in the NeverD workbench"); onClicked: view.backend.openInGui(view.record.objectId) }
    }
    ScrollView {
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
        MarkdownView {
            width: parent.width
            source: view.displayText
            markdown: false; font.family: "JetBrains Mono NL"
            canReference: true; canAskSide: true
            onReferenceRequested: text => view.backend.attachSelection(text, view.record.objectName + " · " + view.record.address)
            onSideQuestionRequested: text => view.backend.createSideChatFromSelection(text)
            onLinkRequested: link => view.backend.openTextLink(link)
        }
    }
    RowLayout {
        visible: !!view.record.id; Layout.fillWidth: true
        Label { text: view.record.complete === false ? qsTr("Partially saved") : qsTr("Saved to the current project"); color: Theme.quiet; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
        ActionButton { visible: view.record.offset > 0; text: qsTr("Back to start"); onClicked: view.backend.openEvidence(view.record.id, 0) }
        ActionButton { visible: view.record.nextOffset >= 0; text: qsTr("Next page"); onClicked: view.backend.openEvidence(view.record.id, view.record.nextOffset) }
    }
    function syncRecord() {
        if (!record.id) return
        objects.currentIndex = (backend.activeProject.objects || []).findIndex(o => o.id === record.objectId)
        query.text = record.address || ""
    }
    Component.onCompleted: syncRecord()
    Connections { target: view.backend; function onEvidenceChanged() { view.syncRecord() } }
}
