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
        Accessible.name: qsTr("证据的分析对象")
    }
    Field { id: query; Layout.fillWidth: true; placeholderText: qsTr("函数名或地址，例如 0x140001000") }
    Flow {
        Layout.fillWidth: true; spacing: 6
        Repeater {
            model: [{id:"c",title:qsTr("伪代码")},{id:"disasm",title:qsTr("反汇编")},{id:"xrefs",title:qsTr("交叉引用")},{id:"cfg",title:qsTr("控制流") }]
            ActionButton {
                required property var modelData
                text: modelData.title
                enabled: !view.backend.reviewBusy && !view.backend.evidenceBusy && !!objects.currentValue && query.text.trim().length > 0
                onClicked: view.backend.inspectObject(objects.currentValue, query.text, modelData.id)
            }
        }
    }
    Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondary; font.pointSize: Theme.smallPointSize
        text: view.backend.evidenceBusy ? qsTr("正在读取并保存证据…") : view.record.id
              ? view.record.objectName + " · " + (view.record.address || view.record.tool) + qsTr("\n分析版本 %1 · %2").arg(view.record.revision || "—").arg(new Date(view.record.createdAt).toLocaleString(Qt.locale(), Locale.ShortFormat))
              : qsTr("读取指定函数，或点击回答中的证据链接。")
    }
    Label { visible: !!view.record.issue; text: view.record.issue || ""; color: Theme.error; wrapMode: Text.Wrap; Layout.fillWidth: true }
    RowLayout {
        visible: !!view.record.id; Layout.fillWidth: true
        ActionButton { text: qsTr("引用到输入"); onClicked: view.backend.attachEvidence(view.record.id) }
        ActionButton { text: qsTr("在 ND GUI 打开"); onClicked: view.backend.openInGui(view.record.objectId) }
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
        Label { text: view.record.complete === false ? qsTr("部分内容已保存") : qsTr("已保存到当前项目"); color: Theme.quiet; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
        ActionButton { visible: view.record.offset > 0; text: qsTr("回到开头"); onClicked: view.backend.openEvidence(view.record.id, 0) }
        ActionButton { visible: view.record.nextOffset >= 0; text: qsTr("下一页"); onClicked: view.backend.openEvidence(view.record.id, view.record.nextOffset) }
    }
    function syncRecord() {
        if (!record.id) return
        objects.currentIndex = (backend.activeProject.objects || []).findIndex(o => o.id === record.objectId)
        query.text = record.address || ""
    }
    Component.onCompleted: syncRecord()
    Connections { target: view.backend; function onEvidenceChanged() { view.syncRecord() } }
}
