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
    border.width: 1
    border.color: Theme.border

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.spaceLarge
        anchors.bottomMargin: Theme.spaceMedium
        anchors.leftMargin: Math.max(24, (parent.width - 800) / 2)
        anchors.rightMargin: Math.max(24, (parent.width - 800) / 2)
        spacing: Theme.spaceLarge
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5
                Label {
                    text: conversation.backend.previewTitle.length > 0
                          ? conversation.backend.previewTitle
                          : conversation.backend.activeProject.name || qsTr("研究工作区")
                    color: Theme.text
                    font.weight: Font.Medium
                    font.pointSize: Theme.bodyPointSize + 2
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                }
                Label {
                    text: conversation.backend.previewTitle.length > 0 ? qsTr("项目文件预览")
                          : conversation.backend.activeId ? qsTr("项目对话  /  %1 个分析对象").arg((conversation.backend.activeProject.objects || []).length)
                          : qsTr("组织对象，连接线索，留下证据")
                    color: Theme.quiet
                    font.pointSize: Theme.smallPointSize
                }
            }
            ActionButton {
                visible: conversation.backend.activeId.length > 0 && conversation.backend.previewTitle.length === 0
                text: conversation.width >= 660 ? qsTr("目标") : ""
                glyph: "bookmark"
                hint: qsTr("设置持续目标")
                checked: conversation.backend.goal.status === "active"
                onClicked: conversation.goalRequested()
            }
            ActionButton {
                visible: conversation.backend.activeId.length > 0 && conversation.backend.previewTitle.length === 0
                glyph: "file_load"
                hint: qsTr("添加分析对象")
                onClicked: conversation.addObjectRequested()
            }
            ActionButton {
                visible: conversation.backend.activeId.length > 0 && conversation.backend.previewTitle.length === 0
                glyph: "folder"
                hint: qsTr("项目文件")
                onClicked: conversation.filesRequested()
            }
            ActionButton {
                visible: conversation.backend.previewTitle.length > 0
                text: qsTr("引用文件")
                onClicked: conversation.backend.attachPreview()
            }
            ActionButton {
                visible: conversation.backend.previewTitle.length > 0
                text: qsTr("返回对话")
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
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width, 540)
                spacing: 18
                Label {
                    text: conversation.backend.activeId ? qsTr("让线索在这里汇合") : qsTr("你的下一项研究，从这里开始")
                    color: Theme.text
                    font.pointSize: Theme.titlePointSize + 4
                    font.weight: Font.Medium
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Label {
                    text: conversation.backend.activeId
                          ? qsTr("添加相关的程序、库或驱动，在同一项目中保存问题、笔记和对象记忆。")
                          : qsTr("一个项目，一处工作目录。把相关分析对象放在一起，留下可以继续推进的研究记录。")
                    color: Theme.secondary
                    font.pointSize: Theme.bodyPointSize
                    wrapMode: Text.Wrap
                    lineHeight: 1.5
                    Layout.fillWidth: true
                }
                RowLayout {
                    spacing: 10
                    Layout.topMargin: 6
                    ActionButton {
                        primary: true
                        text: conversation.backend.activeId ? qsTr("添加分析对象") : qsTr("新建项目")
                        onClicked: conversation.backend.activeId ? conversation.addObjectRequested() : conversation.createRequested()
                    }
                    ActionButton {
                        text: conversation.backend.activeId ? qsTr("项目文件") : qsTr("打开项目")
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
            // Content scrolls under a short fade instead of being cut at a hard edge.
            Rectangle {
                parent: messageList
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                height: 28; z: 1
                visible: !messageList.atYBeginning
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.content }
                    GradientStop { position: 1; color: Qt.rgba(Theme.content.r, Theme.content.g, Theme.content.b, 0) }
                }
            }
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
            border.color: composer.activeFocus ? Theme.focusBorder : Theme.borderStrong
            radius: 14
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
                        placeholderText: qsTr("描述问题，或继续追踪一条线索…")
                        placeholderTextColor: Theme.secondary
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
                    ActionButton { glyph: "plus"; hint: qsTr("添加上下文 · @"); onClicked: conversation.contextRequested() }
                    ChoiceField {
                        id: scope
                        Layout.preferredWidth: Math.min(150, conversation.width / 3)
                        textRole: "name"
                        valueRole: "id"
                        model: [{id: "", name: qsTr("整个项目")}].concat(conversation.backend.activeProject.objects || [])
                        onModelChanged: currentIndex = 0
                        palette.text: Theme.text
                        palette.buttonText: Theme.text
                        palette.button: Theme.raised
                        palette.base: Theme.raised
                        font.pointSize: Theme.smallPointSize
                        Accessible.name: qsTr("本次需求的分析范围")
                    }
                    ActionButton {
                        text: conversation.width >= 660 ? (conversation.backend.modelConfigured ? conversation.backend.modelSettings.model : qsTr("配置模型")) : ""
                        glyph: "options"
                        hint: qsTr("模型设置 · ") + (conversation.backend.modelSettings.model || qsTr("尚未配置"))
                        Layout.maximumWidth: 180
                        font.pointSize: Theme.smallPointSize
                        onClicked: conversation.modelSettingsRequested()
                    }
                    Item { Layout.fillWidth: true }
                    ContextMeter { usage: conversation.backend.contextUsage; compact: conversation.width < 660 }
                    ActionButton {
                        visible: conversation.backend.running
                        glyph: "stop"; hint: qsTr("停止当前任务并暂停队列")
                        onClicked: conversation.backend.cancelRun()
                    }
                    ActionButton {
                        glyph: conversation.backend.running ? "clock" : "send"
                        hint: conversation.backend.running ? qsTr("排到下一轮 · 回车") : qsTr("发送 · 回车")
                        primary: true
                        enabled: composer.canSubmit
                        onClicked: composer.submitRequested()
                    }
                    ActionButton {
                        visible: conversation.backend.running
                        glyph: "chevron_down"; hint: qsTr("发送方式")
                        enabled: composer.text.trim().length > 0
                        onClicked: sendMode.open()
                        ThemedMenu {
                            id: sendMode
                            y: -height - 8
                            ThemedMenuItem { text: qsTr("补充当前任务"); onTriggered: conversation.backend.submitRequest(scope.currentIndex > 0 ? scope.currentValue : "", "steer") }
                            ThemedMenuItem { text: qsTr("排到下一轮"); onTriggered: conversation.backend.submitRequest(scope.currentIndex > 0 ? scope.currentValue : "", "queue") }
                        }
                    }
                }
            }
        }
        RowLayout {
            visible: conversation.backend.previewTitle.length === 0
            Layout.fillWidth: true
            Label {
                text: conversation.approvalVisible ? qsTr("等待审批")
                      : conversation.backend.running ? qsTr("正在分析 · 对象进度可在右侧查看")
                      : qsTr("对话与证据保存在当前项目")
                color: Theme.quiet
                font.pointSize: Theme.smallPointSize
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Label { text: qsTr("回车发送 · Shift+回车换行"); color: Theme.quiet; font.pointSize: Theme.smallPointSize }
        }
    }
    Connections {
        target: conversation.backend
        function onConversationChanged() { composer.text = conversation.backend.draft }
    }
}
