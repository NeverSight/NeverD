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
    spacing: Theme.spaceSmall
    model: messageModel
    ScrollBar.vertical: ThemedScrollBar {}
    delegate: Item {
        id: messageRow
        required property var entry
        required property int index
        readonly property bool fromUser: entry.role === "user"
        // Who spoke shows by where a message stands: the person's in a
        // bubble at the trailing edge, the answer plain across the column.
        // A name is written only where it says more than that.
        readonly property string speaker: (entry.inherited ? qsTr("%1 · Main conversation") : "%1").arg(
            fromUser ? (entry.goalId ? qsTr("Goal") : qsTr("You"))
                     : entry.goalRound > 0 ? qsTr("NeverD · Goal round %1").arg(entry.goalRound) : "NeverD")
        readonly property bool named: !!entry.inherited || !!entry.goalId || entry.goalRound > 0
        readonly property string statusText: ({running: qsTr("Analyzing…"), failed: qsTr("Not completed"), cancelled: qsTr("Stopped"), interrupted: qsTr("The last run was interrupted and not retried automatically."), snapshot: qsTr("Partial reply captured when the side chat was created"), pending: qsTr("Will be added to the task when the current call finishes")}[entry.status] || (entry.inputKind === "steer" ? qsTr("Added to the current task") : ""))
        width: messageList.width
        height: messageColumn.implicitHeight
        Accessible.role: Accessible.Paragraph
        Accessible.name: speaker
        HoverHandler { id: messageHover }
        ColumnLayout {
            id: messageColumn
            width: parent.width
            spacing: Theme.spaceTiny
            Label {
                textFormat: Text.PlainText
                visible: messageRow.named
                text: messageRow.speaker
                color: Theme.quiet
                font.pointSize: Theme.smallPointSize
                Layout.alignment: messageRow.fromUser ? Qt.AlignRight : Qt.AlignLeft
                Layout.leftMargin: messageRow.fromUser ? 0 : 2
                Layout.rightMargin: messageRow.fromUser ? Theme.spaceMedium : 0
            }
            Rectangle {
                id: bubble
                TextMetrics { id: userMetrics; text: messageRow.entry.text; font: messageText.font }
                // The person's words are as wide as they are, within four
                // fifths of the column; an answer takes the column.
                Layout.preferredWidth: messageRow.fromUser
                    ? Math.min(messageRow.width * 0.8, Math.max(64, Math.ceil(userMetrics.advanceWidth) + 2 * Theme.spaceMedium + 2))
                    : messageRow.width
                Layout.alignment: messageRow.fromUser ? Qt.AlignRight : Qt.AlignLeft
                implicitHeight: bubbleContent.implicitHeight + (messageRow.fromUser ? 20 : 0)
                radius: messageRow.fromUser ? Math.min(Theme.largeRadius, height / 2) : 0
                color: messageRow.fromUser ? Theme.userBubble : Theme.assistantBubble
                // High contrast, whose fills are one, outlines the bubble.
                border.width: messageRow.fromUser && Theme.highContrast ? 1 : 0
                border.color: Theme.bubbleBorder
                antialiasing: true
                ColumnLayout {
                    id: bubbleContent
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: messageRow.fromUser ? Theme.spaceMedium : 0
                    anchors.rightMargin: messageRow.fromUser ? Theme.spaceMedium : 0
                    anchors.topMargin: messageRow.fromUser ? 10 : 0
                    spacing: Theme.spaceMedium - Theme.spaceTiny
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
                }
            }
            // Under a message: how it ended, and while the pointer or the
            // keyboard is on it, when it was said and what can be done with it.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: messageRow.fromUser ? 0 : -6
                Layout.rightMargin: messageRow.fromUser ? Theme.spaceSmall : 0
                layoutDirection: messageRow.fromUser ? Qt.RightToLeft : Qt.LeftToRight
                spacing: 2
                readonly property bool shown: messageHover.hovered || copyButton.activeFocus || quoteButton.activeFocus
                                              || copyButton.hovered || quoteButton.hovered || copyTimer.running
                Label {
                    textFormat: Text.PlainText
                    visible: messageRow.statusText.length > 0
                    text: messageRow.statusText
                    color: messageRow.entry.status === "failed" ? Theme.error : Theme.secondary
                    font.pointSize: Theme.smallPointSize
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.leftMargin: 6
                    Layout.rightMargin: 6
                    horizontalAlignment: messageRow.fromUser ? Text.AlignRight : Text.AlignLeft
                }
                ToolButton {
                    id: copyButton
                    property bool copied: false
                    text: copied ? qsTr("Copied") : qsTr("Copy")
                    opacity: parent.shown ? 1 : 0
                    palette.buttonText: Theme.secondary
                    font.pointSize: Theme.smallPointSize
                    padding: 3
                    leftPadding: 6
                    rightPadding: 6
                    background: Rectangle { color: parent.hovered ? Theme.toolHover : "transparent"; radius: Theme.toolRadius - 2 }
                    onClicked: { messageText.copyMessage(); copied = true; copyTimer.restart() }
                    Timer { id: copyTimer; interval: 1500; onTriggered: copyButton.copied = false }
                }
                ToolButton {
                    id: quoteButton
                    visible: messageList.allowQuote && !messageRow.fromUser && messageRow.entry.status !== "running"
                    text: qsTr("Quote")
                    opacity: parent.shown ? 1 : 0
                    palette.buttonText: Theme.secondary
                    font.pointSize: Theme.smallPointSize
                    padding: 3
                    leftPadding: 6
                    rightPadding: 6
                    background: Rectangle { color: parent.hovered ? Theme.toolHover : "transparent"; radius: Theme.toolRadius - 2 }
                    onClicked: messageList.quoteRequested(messageRow.entry.id)
                    ThemedToolTip { visible: parent.hovered; text: qsTr("Quote in the main conversation draft") }
                }
                Label {
                    textFormat: Text.PlainText
                    opacity: parent.shown ? 1 : 0
                    text: new Date(messageRow.entry.createdAt).toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
                    color: Theme.quiet
                    font.pointSize: Theme.smallPointSize
                    Layout.leftMargin: 6
                    Layout.rightMargin: 6
                }
                Item { visible: messageRow.statusText.length === 0; Layout.fillWidth: true }
            }
        }
    }
}
