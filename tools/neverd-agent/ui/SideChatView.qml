import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ColumnLayout {
    id: view
    required property var backend
    signal contextRequested()
    readonly property var chat: backend.sideChat
    readonly property var objectApproval: chat ? backend.analysisObjectApprovals.find(
        request => request.projectId === backend.activeId && request.sideChatId === chat.id) || null : null
    readonly property var chatsWaiting: backend.analysisObjectApprovals.filter(
        request => request.projectId === backend.activeId && !!request.sideChatId)
    property bool showInherited: false
    spacing: 12
    function selectCurrent() {
        syncChooser()
        composer.text = chat ? chat.draft : ""
    }
    function syncChooser() { chooser.currentIndex = chat ? chooser.indexOfValue(chat.id) : -1 }
    onChatChanged: { showInherited = false; Qt.callLater(selectCurrent) }
    Component.onCompleted: selectCurrent()
    RowLayout {
        Layout.fillWidth: true
        ChoiceField {
            id: chooser
            Layout.fillWidth: true
            textRole: "label"; valueRole: "id"
            model: view.backend.sideChats.map(item => ({id: item.id, label:
                view.chatsWaiting.some(request => request.sideChatId === item.id)
                    ? qsTr("%1 · Waiting for approval").arg(item.title) : item.title}))
            onModelChanged: Qt.callLater(view.syncChooser)
            onActivated: view.backend.selectSideChat(currentValue)
            Accessible.name: qsTr("Side chat history")
        }
        ActionButton { glyph: "plus"; hint: qsTr("New side chat from the main conversation"); onClicked: view.backend.createSideChat() }
    }
    ActionButton {
        Layout.fillWidth: true
        glyph: view.showInherited ? "chevron_down" : "chevron_right"
        text: view.chat && view.chat.inheritedMessages.length > 0
              ? qsTr("Inherited from the main conversation · Messages: %1").arg(view.chat.inheritedMessages.length)
              : qsTr("No main conversation history · Starts independently here")
        enabled: view.chat && view.chat.inheritedMessages.length > 0
        font.pointSize: Theme.smallPointSize
        palette.buttonText: Theme.secondary
        background: Rectangle { radius: Theme.toolRadius; color: parent.hovered ? Theme.toolHover : "transparent" }
        onClicked: view.showInherited = !view.showInherited
        ThemedToolTip {
            visible: parent.hovered
            text: view.chat && view.chat.forkedAt
                ? qsTr("Inherited on %1. Both conversations continue separately, and side answers can be quoted back into the main conversation.").arg(new Date(view.chat.forkedAt).toLocaleString(Qt.locale(), Locale.ShortFormat)) : ""
        }
    }
    ChatTranscript {
        backend: view.backend
        sideContext: true
        Layout.fillWidth: true; Layout.fillHeight: true
        compact: true; allowQuote: true
        messages: view.chat ? (view.showInherited ? view.chat.inheritedMessages.concat(view.chat.messages) : view.chat.messages) : []
        onLinkRequested: link => view.backend.openTextLink(link)
        onQuoteRequested: messageId => view.backend.quoteSideAnswer(messageId)
        Label {
            anchors.centerIn: parent; width: parent.width - 24
            visible: (!view.chat || view.chat.messages.length === 0) && !view.showInherited
            text: qsTr("Explore a side question here, with the main conversation's context.")
            horizontalAlignment: Text.AlignHCenter
            color: Theme.secondary; wrapMode: Text.Wrap
        }
    }
    InputQueue {
        Layout.fillWidth: true
        inputs: view.chat ? view.chat.pendingInputs : []
        active: view.chat && view.chat.running
        paused: view.chat && view.chat.queuePaused
        onDiscardRequested: id => view.chat.discardQueuedInput(id)
        onResumeRequested: view.chat.resumeQueue()
    }
    ContextChips {
        Layout.fillWidth: true
        references: view.chat ? view.chat.draftContext : []
        onRemoveRequested: index => view.backend.removeContext(index, true)
    }
    ObjectApprovalCard {
        visible: view.objectApproval !== null
        Layout.fillWidth: true
        Layout.bottomMargin: Theme.spaceSmall - view.spacing
        request: view.objectApproval
        onDecision: (id, allow) => view.backend.resolveAnalysisObjectApproval(id, allow)
    }
    Surface {
        Layout.fillWidth: true; Layout.preferredHeight: 132
        color: Theme.composerTint
        radius: Theme.radius
        border.color: composer.activeFocus ? Theme.focusBorder : Theme.inputBorder
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 14; spacing: Theme.spaceSmall
            ScrollView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                ChatComposer {
                    id: composer
                    objectName: "sideChatComposer"
                    enabled: view.chat !== null
                    placeholderText: qsTr("Another question…")
                    placeholderTextColor: Theme.quiet; color: Theme.text
                    selectByMouse: true; wrapMode: TextEdit.Wrap; background: null
                    onTextChanged: {
                        if (view.chat) view.chat.setDraft(text)
                        if (activeFocus && text.endsWith("@")) { text = text.slice(0, -1); view.contextRequested() }
                    }
                    onSubmitRequested: view.backend.sendSideRequest()
                }
            }
                RowLayout {
                    Layout.fillWidth: true
                    ActionButton { glyph: "plus"; hint: qsTr("Add context…"); onClicked: view.contextRequested() }
                Label {
                    text: view.objectApproval ? qsTr("Waiting for approval")
                          : view.chat && view.chat.running ? qsTr("Analyzing…") : qsTr("Side chat")
                    color: Theme.quiet; font.pointSize: Theme.smallPointSize
                }
                Item { Layout.fillWidth: true }
                ContextMeter { usage: view.chat ? view.chat.contextUsage : ({}) }
                ActionButton { visible: view.chat && view.chat.running; glyph: "stop"; hint: qsTr("Stop and pause the queue"); onClicked: view.chat.cancel() }
                ActionButton {
                    glyph: view.chat && view.chat.running ? "clock" : "send"
                    hint: view.chat && view.chat.running
                          ? qsTr("Enter to queue for the next round · Shift+Enter for a new line")
                          : qsTr("Enter to send · Shift+Enter for a new line")
                    enabled: composer.canSubmit
                    onClicked: composer.submitRequested()
                }
                ActionButton {
                    visible: view.chat && view.chat.running
                    glyph: "chevron_down"; hint: qsTr("Send options"); enabled: composer.text.trim().length > 0
                    onClicked: sideSendMode.open()
                    ThemedMenu {
                        id: sideSendMode; y: -height - 8
                        ThemedMenuItem { text: qsTr("Add to the current task"); onTriggered: view.backend.submitSideRequest("steer") }
                        ThemedMenuItem { text: qsTr("Queue for the next round"); onTriggered: view.backend.submitSideRequest("queue") }
                    }
                }
            }
        }
    }
    Connections { target: view.chat; function onDraftChanged() { composer.text = view.chat.draft } }
}
