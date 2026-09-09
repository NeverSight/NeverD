pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import NeverD.Native 1.0

Rectangle {
    id: root
    property string text: ""
    property string emptyTitle: ""
    property string emptyDetail: ""
    property real codePointSize: Theme.codeSize
    property bool showLineNumbers: true
    property bool syntaxHighlight: false
    property var mappings: []
    property string selectedAddress: ""
    signal sourceLineSelected(int line)
    onSelectedAddressChanged: Qt.callLater(() => {
        const position = highlighter.firstMappedPosition()
        if (position >= 0) code.cursorPosition = position
    })
    color: Theme.editor
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true
    // Documents are a bounded, current-function result supplied by the worker.
    // A single native text control preserves text selection, clipboard and IME semantics.
    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: 0
        visible: root.text.length > 0
        clip: true
        contentWidth: Math.max(availableWidth, code.implicitWidth)
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.active: true
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.horizontal.active: true
        TextArea {
            id: code
            objectName: "codeText"
            text: root.text
            readOnly: true
            width: Math.max(scroll.availableWidth, implicitWidth)
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            textFormat: TextEdit.PlainText
            font.family: Theme.monoFont
            font.pointSize: root.codePointSize
            color: Theme.foreground
            selectionColor: Theme.selection
            selectedTextColor: Theme.accentForeground
            leftPadding: 18
            rightPadding: 24
            topPadding: 18
            bottomPadding: 24
            background: Rectangle { color: Theme.editor }
            Accessible.name: root.emptyTitle
            property string previousText: ""
            property int previousCursor: 0
            onCursorPositionChanged: if (text === previousText) previousCursor = cursorPosition
            onTextChanged: {
                const appended = previousText.length > 0 && text.indexOf(previousText) === 0
                const savedX = scroll.contentItem.contentX
                const savedY = scroll.contentItem.contentY
                cursorPosition = appended ? Math.min(previousCursor, length) : 0
                previousText = text
                if (appended) Qt.callLater(() => { scroll.contentItem.contentX = savedX; scroll.contentItem.contentY = savedY })
            }
            NativeCodeHighlighter { id: highlighter; document: code.textDocument; enabled: root.syntaxHighlight; mappings: root.mappings; selectedAddress: root.selectedAddress }
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onSingleTapped: eventPoint => {
                    if (root.mappings.length > 0) root.sourceLineSelected(highlighter.lineAtPosition(code.positionAt(eventPoint.position.x, eventPoint.position.y)))
                }
            }
        }
    }
    EmptyPane { anchors.fill: parent; visible: root.text.length === 0; title: root.emptyTitle; detail: root.emptyDetail }
    function copySelection() { code.copy() }
}
