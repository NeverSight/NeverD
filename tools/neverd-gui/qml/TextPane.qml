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
    property bool anchorPending: false
    property var appendViewport: null
    property point readingViewport: Qt.point(0, 0)
    function rememberViewport() {
        // TextArea can reset its cursor and scroll before onTextChanged runs.
        // Only record scrolling while the displayed document is stable.
        if (text === code.text && code.text === code.previousText)
            readingViewport = Qt.point(scroll.contentItem.contentX, scroll.contentItem.contentY)
    }
    Connections {
        target: scroll.contentItem
        function onContentXChanged() { root.rememberViewport() }
        function onContentYChanged() { root.rememberViewport() }
    }
    function requestAnchor() {
        anchorPending = true
        Qt.callLater(applyReadingPosition)
    }
    function applyReadingPosition() {
        const viewport = scroll.contentItem
        if (appendViewport !== null) {
            viewport.contentX = appendViewport.x
            viewport.contentY = appendViewport.y
            appendViewport = null
        }
        // Read the latest snapshot here: a queued restore must never retain an
        // address, mapping row or document position from an earlier result.
        if (!anchorPending || text.length === 0 || mappings.length === 0 || selectedAddress.length === 0)
            return
        const position = highlighter.firstMappedPosition()
        if (position < 0)
            return
        code.cursorPosition = position
        if (viewport.width <= 0 || viewport.height <= 0 || code.cursorRectangle.height <= 0)
            return
        const cursor = code.mapToItem(viewport, code.cursorRectangle.x, code.cursorRectangle.y)
        const bottom = cursor.y + code.cursorRectangle.height
        const right = cursor.x + code.cursorRectangle.width
        const dx = cursor.x < 0 ? cursor.x : Math.max(0, right - viewport.width)
        const dy = cursor.y < 0 ? cursor.y : Math.max(0, bottom - viewport.height)
        viewport.contentX = Math.max(0, Math.min(viewport.contentX + dx, viewport.contentWidth - viewport.width))
        viewport.contentY = Math.max(0, Math.min(viewport.contentY + dy, viewport.contentHeight - viewport.height))
        anchorPending = false
    }
    onSelectedAddressChanged: requestAnchor()
    onMappingsChanged: if (anchorPending) Qt.callLater(applyReadingPosition)
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
            onCursorRectangleChanged: if (root.anchorPending) Qt.callLater(root.applyReadingPosition)
            onTextChanged: {
                const appended = previousText.length > 0 && text.indexOf(previousText) === 0
                if (appended) {
                    if (root.appendViewport === null)
                        root.appendViewport = root.readingViewport
                } else {
                    // Replacements retire any deferred append restoration.
                    root.appendViewport = null
                    root.anchorPending = true
                }
                cursorPosition = appended ? Math.min(previousCursor, length) : 0
                previousCursor = cursorPosition
                previousText = text
                root.rememberViewport()
                Qt.callLater(root.applyReadingPosition)
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
