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
                item.title + (view.chatsWaiting.some(request => request.sideChatId === item.id)
                    ? qsTr(" · 待审批") : "")}))
            onModelChanged: Qt.callLater(view.syncChooser)
            onActivated: view.backend.selectSideChat(currentValue)
            Accessible.name: qsTr("侧边聊天历史")
        }
        ActionButton { glyph: "plus"; hint: qsTr("从当前主对话新建侧边聊天"); onClicked: view.backend.createSideChat() }
    }
    ActionButton {
        Layout.fillWidth: true
        glyph: view.showInherited ? "chevron_down" : "chevron_right"
        text: view.chat && view.chat.inheritedMessages.length > 0
              ? qsTr("已继承主对话 · %1 条消息").arg(view.chat.inheritedMessages.length)
              : qsTr("主对话暂无历史 · 在这里独立展开")
        enabled: view.chat && view.chat.inheritedMessages.length > 0
        font.pointSize: Theme.smallPointSize
        palette.buttonText: Theme.secondary
        background: Rectangle { radius: Theme.toolRadius; color: parent.hovered ? Theme.toolHover : "transparent" }
        onClicked: view.showInherited = !view.showInherited
        ThemedToolTip {
            visible: parent.hovered
            text: view.chat && view.chat.forkedAt
                ? qsTr("继承于 %1。后续两边独立继续，侧边回答可引用回主对话。").arg(new Date(view.chat.forkedAt).toLocaleString(Qt.locale(), Locale.ShortFormat)) : ""
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
            text: qsTr("带着主对话的上下文，\n在这里继续聊一个支线问题。")
            horizontalAlignment: Text.AlignHCenter
            color: Theme.secondary; wrapMode: Text.Wrap; lineHeight: 1.5
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
                    placeholderText: qsTr("另一个问题…")
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
                    ActionButton { glyph: "plus"; hint: qsTr("添加上下文"); onClicked: view.contextRequested() }
                Label {
                    text: view.objectApproval ? qsTr("等待审批")
                          : view.chat && view.chat.running ? qsTr("分析中…") : qsTr("侧边聊天")
                    color: Theme.quiet; font.pointSize: Theme.smallPointSize
                }
                Item { Layout.fillWidth: true }
                ContextMeter { usage: view.chat ? view.chat.contextUsage : ({}) }
                ActionButton { visible: view.chat && view.chat.running; glyph: "stop"; hint: qsTr("停止并暂停队列"); onClicked: view.chat.cancel() }
                ActionButton {
                    glyph: view.chat && view.chat.running ? "clock" : "send"
                    hint: view.chat && view.chat.running
                          ? qsTr("回车排到下一轮 · Shift+回车换行")
                          : qsTr("回车发送 · Shift+回车换行")
                    enabled: composer.canSubmit
                    onClicked: composer.submitRequested()
                }
                ActionButton {
                    visible: view.chat && view.chat.running
                    glyph: "chevron_down"; hint: qsTr("发送方式"); enabled: composer.text.trim().length > 0
                    onClicked: sideSendMode.open()
                    ThemedMenu {
                        id: sideSendMode; y: -height - 8
                        ThemedMenuItem { text: qsTr("补充当前任务"); onTriggered: view.backend.submitSideRequest("steer") }
                        ThemedMenuItem { text: qsTr("排到下一轮"); onTriggered: view.backend.submitSideRequest("queue") }
                    }
                }
            }
        }
    }
    Connections { target: view.chat; function onDraftChanged() { composer.text = view.chat.draft } }
}
