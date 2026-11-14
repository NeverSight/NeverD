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
        anchors.topMargin: Theme.spaceLarge
        anchors.bottomMargin: Theme.spaceMedium
        anchors.leftMargin: Math.max(24, (parent.width - 800) / 2)
        anchors.rightMargin: Math.max(24, (parent.width - 800) / 2)
        spacing: Theme.spaceLarge
        RowLayout {
            // Without a project, the empty state below says what this is.
            visible: conversation.backend.activeId.length > 0 || conversation.backend.previewTitle.length > 0
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5
                Label {
                    text: conversation.backend.previewTitle.length > 0
                          ? conversation.backend.previewTitle
                          : conversation.backend.activeProject.name || qsTr("Research workspace")
                    color: Theme.text
                    font.weight: Font.Medium
                    font.pointSize: Theme.bodyPointSize + 2
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                }
                Label {
                    text: conversation.backend.previewTitle.length > 0 ? qsTr("Project file preview")
                          : conversation.backend.activeId ? qsTr("Project conversation  /  Analysis objects: %1").arg((conversation.backend.activeProject.objects || []).length)
                          : qsTr("Organize objects, connect leads, keep evidence")
                    color: Theme.quiet
                    font.pointSize: Theme.smallPointSize
                }
            }
            ActionButton {
                visible: conversation.backend.activeId.length > 0 && conversation.backend.previewTitle.length === 0
                text: conversation.width >= 660 ? qsTr("Goal") : ""
                glyph: "bookmark"
                hint: qsTr("Set a persistent goal…")
                checked: conversation.backend.goal.status === "active"
                onClicked: conversation.goalRequested()
            }
            ActionButton {
                visible: conversation.backend.activeId.length > 0 && conversation.backend.previewTitle.length === 0
                glyph: "file_load"
                hint: qsTr("Add analysis object…")
                onClicked: conversation.addObjectRequested()
            }
            ActionButton {
                visible: conversation.backend.activeId.length > 0 && conversation.backend.previewTitle.length === 0
                glyph: "folder"
                hint: qsTr("Project files")
                onClicked: conversation.filesRequested()
            }
            ActionButton {
                visible: conversation.backend.previewTitle.length > 0
                text: qsTr("Attach file")
                onClicked: conversation.backend.attachPreview()
            }
            ActionButton {
                visible: conversation.backend.previewTitle.length > 0
                text: qsTr("Back to conversation")
                onClicked: conversation.backend.closePreview()
            }
        }
        Rectangle {
            visible: conversation.backend.activeId.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            Layout.topMargin: -Theme.spaceSmall
            color: Theme.border
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
            // On the conversation's left edge, as the header and messages
            // are, a little above the middle.
            ColumnLayout {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: -parent.height / 10
                width: Math.min(parent.width, 560)
                spacing: 12
                Label {
                    text: conversation.backend.activeId ? qsTr("Bring your leads together here") : qsTr("Your next research project starts here")
                    color: Theme.text
                    // The workbench's start page sets its empty-state line a
                    // quarter above its text.
                    font.pointSize: Theme.bodyPointSize * 1.25
                    font.weight: Font.Medium
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Label {
                    text: conversation.backend.activeId
                          ? qsTr("Add related programs, libraries or drivers, and keep questions, notes and object memory in one project.")
                          : qsTr("One project, one working directory. Keep related analysis objects together and leave a research log you can build on.")
                    color: Theme.secondary
                    font.pointSize: Theme.bodyPointSize
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                RowLayout {
                    spacing: 10
                    Layout.topMargin: 6
                    ActionButton {
                        primary: true
                        text: conversation.backend.activeId ? qsTr("Add analysis object…") : qsTr("New project")
                        onClicked: conversation.backend.activeId ? conversation.addObjectRequested() : conversation.createRequested()
                    }
                    ActionButton {
                        text: conversation.backend.activeId ? qsTr("Project files") : qsTr("Open project…")
                        glyph: "open"
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
            Layout.bottomMargin: Theme.spaceSmall - Theme.spaceLarge
            request: conversation.objectApproval
            onDecision: (id, allow) => conversation.backend.resolveAnalysisObjectApproval(id, allow)
        }
        Surface {
            visible: conversation.backend.activeId.length > 0 && conversation.backend.previewTitle.length === 0
            Layout.fillWidth: true
            Layout.preferredHeight: 104
            color: Theme.composerTint
            border.color: composer.activeFocus ? Theme.focusBorder : Theme.inputBorder
            radius: Theme.radius
            border.width: 1
            Behavior on border.color { ColorAnimation { duration: 140 } }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceMedium
                spacing: Theme.spaceSmall
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    ChatComposer {
                        id: composer
                        objectName: "requestComposer"
                        placeholderText: qsTr("Describe a question, or follow a lead…")
                        placeholderTextColor: Theme.quiet
                        color: Theme.text
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        font.pointSize: Theme.basePointSize
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
                        Layout.preferredWidth: Math.min(150, conversation.width / 3)
                        textRole: "name"
                        valueRole: "id"
                        model: [{id: "", name: qsTr("Whole project")}].concat(conversation.backend.activeProject.objects || [])
                        onModelChanged: currentIndex = 0
                        palette.text: Theme.text
                        palette.buttonText: Theme.text
                        palette.button: Theme.raised
                        palette.base: Theme.raised
                        font.pointSize: Theme.smallPointSize
                        Accessible.name: qsTr("Analysis scope for this request")
                    }
                    ActionButton {
                        text: conversation.width >= 660 ? (conversation.backend.modelConfigured ? conversation.backend.modelSettings.model : qsTr("Configure model…")) : ""
                        glyph: "options"
                        hint: qsTr("Model settings… · %1").arg(conversation.backend.modelSettings.model || qsTr("Not configured"))
                        Layout.maximumWidth: 180
                        font.pointSize: Theme.smallPointSize
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
            Label {
                text: conversation.approvalVisible ? qsTr("Waiting for approval")
                      : conversation.backend.running ? qsTr("Analyzing · Object progress is in the sidebar.")
                      : qsTr("Conversations and evidence are saved in the current project.")
                color: Theme.quiet
                font.pointSize: Theme.smallPointSize
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            // The key hint gives way first when the column narrows.
            Label {
                visible: conversation.width >= 760
                text: qsTr("Enter to send · Shift+Enter for a new line"); color: Theme.quiet; font.pointSize: Theme.smallPointSize
            }
        }
    }
    Connections {
        target: conversation.backend
        function onConversationChanged() { composer.text = conversation.backend.draft }
    }
}
