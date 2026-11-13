import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

TextEdit {
    id: view
    property string source: ""
    property bool markdown: true
    property bool streaming: false
    property bool ready: false
    signal linkRequested(string link)
    property bool canReference: false
    property bool canAskSide: false
    signal referenceRequested(string text)
    signal sideQuestionRequested(string text)
    readOnly: true
    selectByMouse: true
    persistentSelection: true
    wrapMode: TextEdit.Wrap
    color: Theme.text
    selectedTextColor: Theme.text
    selectionColor: Theme.selected
    font.pointSize: Theme.bodyPointSize
    font.family: appFont.family
    textFormat: TextEdit.RichText
    Accessible.role: Accessible.StaticText
    function refresh() {
        const first = selectionStart
        const last = selectionEnd
        renderer.render(source, markdown, Theme.text, Theme.code, Theme.accent, Theme.border, view.font)
        if (last > first) select(Math.min(first, length), Math.min(last, length))
    }
    function schedule() {
        if (!ready) return
        if (streaming) {
            if (!renderTimer.running) renderTimer.start()
        } else {
            renderTimer.stop()
            refresh()
        }
    }
    function copyMessage() { renderer.copySource() }
    onSourceChanged: schedule()
    onFontChanged: schedule()
    onMarkdownChanged: schedule()
    onStreamingChanged: schedule()
    Component.onCompleted: { ready = true; refresh() }
    onLinkActivated: link => linkRequested(link)
    MarkdownDocument { id: renderer; document: view.textDocument }
    Timer { id: renderTimer; interval: 100; onTriggered: view.refresh() }
    Connections { target: Theme; function onLightChanged() { view.refresh() } }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: mouse => {
            messageMenu.codePosition = view.positionAt(mouse.x, mouse.y)
            messageMenu.popup()
        }
    }
    ThemedMenu {
        id: messageMenu
        property int codePosition: 0
        ThemedMenuItem { text: qsTr("复制选中文字"); enabled: view.selectedText.length > 0; onTriggered: view.copy() }
        ThemedMenuItem { text: qsTr("复制代码块"); enabled: renderer.hasCodeAt(messageMenu.codePosition); onTriggered: renderer.copyCodeAt(messageMenu.codePosition) }
        ThemedMenuItem { text: qsTr("复制原文"); onTriggered: view.copyMessage() }
        ThemedSeparator { visible: view.canReference || view.canAskSide }
        ThemedMenuItem { visible: view.canReference; text: qsTr("引用到输入"); enabled: view.selectedText.length > 0; onTriggered: view.referenceRequested(view.selectedText.replace(/\u2029/g, "\n")) }
        ThemedMenuItem { visible: view.canAskSide; text: qsTr("在侧边聊天中追问"); enabled: view.selectedText.length > 0; onTriggered: view.sideQuestionRequested(view.selectedText.replace(/\u2029/g, "\n")) }
    }
}
