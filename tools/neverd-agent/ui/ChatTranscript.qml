import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ListView {
    id: messageList
    property var messages: []
    property bool compact: false
    property bool allowQuote: false
    property var backend: null
    property bool sideContext: false
    signal linkRequested(string link)
    signal quoteRequested(string messageId)
    ListModel { id: messageModel }
    function refreshMessages() {
        const items = messages
        let reset = messageModel.count > items.length
        for (let i = 0; !reset && i < messageModel.count; ++i)
            reset = messageModel.get(i).entry.id !== items[i].id
        const follow = reset || messageModel.count === 0 || messageList.followTail
        if (reset) messageList.followTail = true
        if (reset) messageModel.clear()
        // Insert new messages in one batch so the view lays out once, not once per row.
        const known = messageModel.count
        if (items.length > known) messageModel.append(items.slice(known).map(entry => ({entry: entry})))
        for (let i = 0; i < known; ++i) {
            const old = messageModel.get(i).entry
            if (old.text !== items[i].text || old.status !== items[i].status || old.runId !== items[i].runId)
                messageModel.setProperty(i, "entry", items[i])
        }
        if (follow) followTimer.restart()
    }
    Component.onCompleted: refreshMessages()
    onMessagesChanged: refreshMessages()
    onHeightChanged: if (followTail) followTimer.restart()

    property bool followTail: true
    Timer {
        id: followTimer
        interval: 0
        onTriggered: {
            if (messageList.followTail && !messageList.atYEnd) messageList.positionViewAtEnd()
        }
    }
    onMovementStarted: followTail = false
    onMovementEnded: followTail = atYEnd
    clip: true
    spacing: Theme.spaceLarge
    model: messageModel
    ScrollBar.vertical: ThemedScrollBar {}
    delegate: Item {
        id: messageRow
        required property var entry
        required property int index
        readonly property bool fromUser: entry.role === "user"
        width: messageList.width
        height: bubble.height + 4
        HoverHandler { id: messageHover }
        Rectangle {
            id: bubble
            width: messageRow.fromUser ? Math.min(messageRow.width * 0.82, Math.max(210, userMetrics.advanceWidth + 40)) : messageRow.width - (messageList.compact ? 4 : 30)
            x: messageRow.fromUser ? messageRow.width - width - 6 : 0
            height: bubbleContent.implicitHeight + 32
            radius: Theme.radius
            color: messageRow.fromUser ? Theme.userBubble : Theme.assistantBubble
            border.width: messageRow.fromUser ? 1 : 0
            border.color: Theme.bubbleBorder
            TextMetrics { id: userMetrics; text: messageRow.entry.text; font: messageText.font }
            ColumnLayout {
                id: bubbleContent
                anchors.left: parent.left; anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 16
                spacing: Theme.spaceMedium
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 7
                    Rectangle {
                        visible: !messageRow.fromUser
                        width: 6; height: 6; radius: 3
                        color: messageRow.entry.status === "running" ? Theme.accent : Theme.accentLine
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Label {
                        text: (messageRow.entry.inherited ? qsTr("%1 · Main conversation") : "%1").arg(messageRow.fromUser ? (messageRow.entry.goalId ? qsTr("Goal") : qsTr("You")) : messageRow.entry.goalRound > 0 ? qsTr("NeverD · Goal round %1").arg(messageRow.entry.goalRound) : "NeverD")
                        color: Theme.quiet
                        font.weight: Font.Medium
                        font.pointSize: Theme.smallPointSize
                    }
                    Label {
                        text: "· " + new Date(messageRow.entry.createdAt).toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
                        color: Theme.secondary
                        font.pointSize: Theme.smallPointSize
                    }
                    Item { Layout.fillWidth: true }
                    ToolButton {
                        visible: messageList.allowQuote && !messageRow.fromUser && messageRow.entry.status !== "running"
                        text: qsTr("Quote")
                        palette.buttonText: Theme.secondary
                        font.pointSize: Theme.smallPointSize
                        padding: 2
                        background: Rectangle { color: parent.hovered ? Theme.toolHover : "transparent"; radius: Theme.toolRadius }
                        onClicked: messageList.quoteRequested(messageRow.entry.id)
                        ThemedToolTip { visible: parent.hovered; text: qsTr("Quote in the main conversation draft") }
                    }
                    ToolButton {
                        property bool copied: false
                        text: copied ? qsTr("Copied") : qsTr("Copy")
                        opacity: messageHover.hovered || activeFocus || hovered || copyTimer.running ? 1 : 0
                        palette.buttonText: Theme.secondary
                        font.pointSize: Theme.smallPointSize
                        padding: 2
                        background: Rectangle { color: parent.hovered ? Theme.toolHover : "transparent"; radius: Theme.toolRadius }
                        onClicked: { messageText.copyMessage(); copied = true; copyTimer.restart() }
                        Timer { id: copyTimer; interval: 1500; onTriggered: parent.copied = false }
                    }
                }
                MarkdownView {
                    id: messageText
                    Layout.fillWidth: true
                    source: messageRow.entry.text
                    canReference: messageList.backend !== null
                    canAskSide: messageList.backend !== null && !messageList.sideContext
                    onReferenceRequested: text => messageList.backend.attachSelection(text, qsTr("Conversation selection"), messageList.sideContext)
                    onSideQuestionRequested: text => messageList.backend.createSideChatFromSelection(text)
                    streaming: messageRow.entry.status === "running"
                    onLinkRequested: link => messageList.linkRequested(link)
                    onContentHeightChanged: {
                        if (messageList.followTail && messageRow.index === messageList.count - 1) followTimer.restart()
                    }
                }
                ExecutionView {
                    visible: !!messageRow.entry.runId && !messageRow.fromUser && messageList.backend !== null
                    Layout.fillWidth: true
                    backend: messageList.backend
                    runId: messageRow.entry.runId || ""
                    active: messageRow.entry.status === "running"
                    onOpened: messageList.positionViewAtIndex(messageRow.index, ListView.End)
                }
                Label {
                    text: ({running: qsTr("Analyzing…"), failed: qsTr("Not completed"), cancelled: qsTr("Stopped"), interrupted: qsTr("The last run was interrupted and not retried automatically."), snapshot: qsTr("Partial reply captured when the side chat was created"), pending: qsTr("Will be added to the task when the current call finishes")}[messageRow.entry.status] || (messageRow.entry.inputKind === "steer" ? qsTr("Added to the current task") : ""))
                    visible: text.length > 0
                    color: messageRow.entry.status === "failed" ? Theme.error : Theme.secondary
                    font.pointSize: Theme.smallPointSize
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
        }
    }
}
