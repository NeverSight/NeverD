import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Surface {
    id: conversation
    required property var backend
    readonly property var objectApproval: backend.analysisObjectApprovals.find(
        request => request.projectId === backend.activeId && !request.sideChatId) || null
    readonly property bool approvalVisible: objectApproval !== null && backend.previewTitle.length === 0
    signal createRequested()
    signal openRequested()
    signal addObjectRequested()
    signal filesRequested()
    signal modelSettingsRequested()
    signal goalRequested()
    signal contextRequested()
    color: Theme.content
    radius: Theme.workspaceRadius

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.spaceSmall
        anchors.bottomMargin: Theme.spaceMedium - Theme.spaceTiny
        // One reading column, in the middle of the pane.
        anchors.leftMargin: Math.max(Theme.spaceLarge, (parent.width - Theme.readingWidth) / 2)
        anchors.rightMargin: Math.max(Theme.spaceLarge, (parent.width - Theme.readingWidth) / 2)
        spacing: Theme.spaceMedium
        RowLayout {
            // The window's top strip names the project; a file being read
            // names itself here, with the ways back.
            visible: conversation.backend.previewTitle.length > 0
            Layout.fillWidth: true
            spacing: Theme.spaceSmall
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: conversation.backend.previewTitle
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.weight: Font.Medium
                    font.pointSize: Theme.titlePointSize
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                }
                Label {
                    text: qsTr("Project file preview")
                    color: Theme.quiet
                    font.pointSize: Theme.smallPointSize
                }
            }
            ActionButton {
                text: qsTr("Attach file")
                onClicked: conversation.backend.attachPreview()
            }
            ActionButton {
                text: qsTr("Back to conversation")
                onClicked: conversation.backend.closePreview()
            }
        }
        GoalStrip {
            visible: !!conversation.backend.goal.id && conversation.backend.previewTitle.length === 0
            Layout.fillWidth: true
            backend: conversation.backend
            onEditRequested: conversation.goalRequested()
        }
        ScrollView {
            visible: conversation.backend.previewTitle.length > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            MarkdownView {
                width: parent.width
                source: conversation.backend.previewText
                markdown: /\.(md|markdown)$/i.test(conversation.backend.previewTitle)
                onLinkRequested: link => conversation.backend.openTextLink(link)
                canReference: true; canAskSide: true
                onReferenceRequested: text => conversation.backend.attachSelection(text, conversation.backend.previewTitle)
                onSideQuestionRequested: text => conversation.backend.createSideChatFromSelection(text)
            }
        }
        Item {
            visible: conversation.backend.previewTitle.length === 0 && conversation.backend.messages.length === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            // What the window is for, in its one large line, in the middle
            // of the pane and a little above it.
            ColumnLayout {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: -parent.height / 12
                width: Math.min(parent.width, 600)
                spacing: Theme.spaceMedium - Theme.spaceTiny
                Image {
                    visible: !conversation.backend.activeId
                    source: "qrc:/agent/icons/brand.svg"
                    sourceSize: Qt.size(48, 48)
                    Layout.preferredWidth: 48
                    Layout.preferredHeight: 48
                    Layout.alignment: Qt.AlignHCenter
                    Layout.bottomMargin: Theme.spaceSmall
                }
                Label {
                    text: conversation.backend.activeId ? qsTr("Bring your leads together here") : qsTr("Your next research project starts here")
                    color: Theme.text
                    font.pointSize: Theme.heroPointSize
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Label {
                    text: conversation.backend.activeId
                          ? qsTr("Add related programs, libraries or drivers, and keep questions, notes and object memory in one project.")
                          : qsTr("One project, one working directory. Keep related analysis objects together and leave a research log you can build on.")
                    color: Theme.secondary
                    font.pointSize: Theme.bodyPointSize
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.maximumWidth: 480
                    Layout.alignment: Qt.AlignHCenter
                }
                RowLayout {
                    spacing: Theme.spaceSmall
                    Layout.topMargin: Theme.spaceSmall
                    Layout.alignment: Qt.AlignHCenter
                    ActionButton {
                        // The window's main button until it has something
                        // to send: then the composer's is.
                        primary: !conversation.backend.activeId || (conversation.backend.activeProject.objects || []).length === 0
                        text: conversation.backend.activeId ? qsTr("Add analysis object…") : qsTr("New project")
                        onClicked: conversation.backend.activeId ? conversation.addObjectRequested() : conversation.createRequested()
                    }
                    ActionButton {
                        text: conversation.backend.activeId ? qsTr("Project files") : qsTr("Open project…")
                        onClicked: conversation.backend.activeId ? conversation.filesRequested() : conversation.openRequested()
                    }
                }
            }
        }
        ChatTranscript {
            id: messageList
            backend: conversation.backend
            visible: conversation.backend.previewTitle.length === 0 && conversation.backend.messages.length > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            messages: conversation.backend.messages
            onLinkRequested: link => conversation.backend.openTextLink(link)
        }
        InputQueue {
            Layout.fillWidth: true
            inputs: conversation.backend.pendingInputs
            active: conversation.backend.running
            paused: conversation.backend.queuePaused
            onDiscardRequested: inputId => conversation.backend.discardQueuedInput(inputId)
            onResumeRequested: conversation.backend.resumeInputQueue()
        }
        ContextChips {
            Layout.fillWidth: true
            references: conversation.backend.draftContext
            onRemoveRequested: index => conversation.backend.removeContext(index)
        }
        ObjectApprovalCard {
            visible: conversation.approvalVisible
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.spaceSmall - Theme.spaceMedium
            request: conversation.objectApproval
            onDecision: (id, allow) => conversation.backend.resolveAnalysisObjectApproval(id, allow)
        }
        Rectangle {
            id: composerBox
            visible: conversation.backend.activeId.length > 0 && conversation.backend.previewTitle.length === 0
            Layout.fillWidth: true
            implicitHeight: composerColumn.implicitHeight + 2 * composerColumn.anchors.margins
            // The window's main object: a floating surface's fill and
            // outline, the focus color's outline while it is typed in.
            color: Theme.menu
            radius: Theme.largeRadius
            border.width: 1
            border.color: composer.activeFocus ? Theme.focusBorder : Theme.borderStrong
            antialiasing: true
            Behavior on border.color { ColorAnimation { duration: 140 } }
            ColumnLayout {
                id: composerColumn
                anchors.fill: parent
                anchors.margins: 12
                spacing: Theme.spaceSmall
                ScrollView {
                    Layout.fillWidth: true
                    // Two lines to begin with, eight before it scrolls, by
                    // the lines as they are laid out: a line of mixed
                    // scripts is taller than the font's.
                    Layout.preferredHeight: Math.min(Math.max(composer.implicitHeight, Math.ceil(composer.linePitch * 2) + 8),
                                                     Math.ceil(composer.linePitch * 8) + 8)
                    clip: true
                    ChatComposer {
                        id: composer
                        objectName: "requestComposer"
                        readonly property real linePitch: contentHeight / Math.max(1, lineCount)
                        placeholderText: qsTr("Describe a question, or follow a lead…")
                        placeholderTextColor: Theme.quiet
                        color: Theme.text
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        font.pointSize: Theme.basePointSize
                        topPadding: 4
                        bottomPadding: 4
                        leftPadding: 6
                        rightPadding: 6
                        background: null
                        onTextChanged: {
                            conversation.backend.setDraft(text)
                            if (activeFocus && text.endsWith("@")) {
                                text = text.slice(0, -1)
                                conversation.contextRequested()
                            }
                        }
                        Component.onCompleted: text = conversation.backend.draft
                        onSubmitRequested: conversation.backend.sendRequest(scope.currentIndex > 0 ? scope.currentValue : "")
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    ActionButton { glyph: "plus"; hint: qsTr("Add context… · @"); onClicked: conversation.contextRequested() }
                    ChoiceField {
                        id: scope
                        quiet: true
                        // As wide as the chosen scope's name, so a longer
                        // language does not cut it, within a third of the row.
                        TextMetrics { id: scopeText; font: scope.font; text: scope.displayText }
                        Layout.preferredWidth: Math.min(Math.ceil(scopeText.width) + scope.leftPadding + scope.rightPadding + 4,
                                                        conversation.width / 3)
                        textRole: "name"
                        valueRole: "id"
                        model: [{id: "", name: qsTr("Whole project")}].concat(conversation.backend.activeProject.objects || [])
                        onModelChanged: currentIndex = 0
                        palette.text: Theme.text
                        palette.buttonText: Theme.text
                        palette.button: Theme.raised
                        palette.base: Theme.raised
                        Accessible.name: qsTr("Analysis scope for this request")
                    }
                    ActionButton {
                        text: conversation.width >= 660 ? (conversation.backend.modelConfigured ? conversation.backend.modelSettings.model : qsTr("Configure model…")) : ""
                        glyph: "options"
                        hint: qsTr("Model settings… · %1").arg(conversation.backend.modelSettings.model || qsTr("Not configured"))
                        Layout.maximumWidth: 200
                        onClicked: conversation.modelSettingsRequested()
                    }
                    Item { Layout.fillWidth: true }
                    ContextMeter { usage: conversation.backend.contextUsage; compact: conversation.width < 660 }
                    ActionButton {
                        visible: conversation.backend.running
                        glyph: "stop"; hint: qsTr("Stop the current task and pause the queue")
                        onClicked: conversation.backend.cancelRun()
                    }
                    ActionButton {
                        glyph: conversation.backend.running ? "clock" : "send"
                        hint: conversation.backend.running ? qsTr("Queue for the next round · Enter") : qsTr("Send · Enter")
                        primary: true
                        round: true
                        enabled: composer.canSubmit
                        onClicked: composer.submitRequested()
                    }
                    ActionButton {
                        visible: conversation.backend.running
                        glyph: "chevron_down"; hint: qsTr("Send options")
                        enabled: composer.text.trim().length > 0
                        onClicked: sendMode.open()
                        ThemedMenu {
                            id: sendMode
                            y: -height - 8
                            ThemedMenuItem { text: qsTr("Add to the current task"); onTriggered: conversation.backend.submitRequest(scope.currentIndex > 0 ? scope.currentValue : "", "steer") }
                            ThemedMenuItem { text: qsTr("Queue for the next round"); onTriggered: conversation.backend.submitRequest(scope.currentIndex > 0 ? scope.currentValue : "", "queue") }
                        }
                    }
                }
            }
        }
        RowLayout {
            // Hints for the composer, which a project shows.
            visible: conversation.backend.activeId.length > 0 && conversation.backend.previewTitle.length === 0
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceMedium - Theme.spaceTiny
            Layout.rightMargin: Theme.spaceMedium - Theme.spaceTiny
            Layout.topMargin: -Theme.spaceSmall
            Label {
                id: composerStatus
                text: conversation.approvalVisible ? qsTr("Waiting for approval")
                      : conversation.backend.running ? qsTr("Analyzing · Object progress is in the sidebar.")
                      : qsTr("Conversations and evidence are saved in the current project.")
                color: Theme.quiet
                font.pointSize: Theme.smallPointSize
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            // The key hint gives way first when both do not fit.
            Label {
                visible: composerStatus.implicitWidth + implicitWidth + parent.spacing <= parent.width
                text: qsTr("Enter to send · Shift+Enter for a new line"); color: Theme.quiet; font.pointSize: Theme.smallPointSize
            }
        }
    }
    Connections {
        target: conversation.backend
        function onConversationChanged() { composer.text = conversation.backend.draft }
    }
}
