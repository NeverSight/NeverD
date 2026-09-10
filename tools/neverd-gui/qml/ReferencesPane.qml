pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property var controller
    property var kddockwidgets_min_size: Qt.size(280, 100)
    readonly property real addressWidth: Math.ceil(addressMetrics.advanceWidth("0xffffffffffffffff"))
    readonly property bool compact: width < addressWidth * 2 + 160
    readonly property real referenceRowHeight: Math.max(Theme.rowHeight, Math.ceil(addressMetrics.height * (compact ? 2 : 1) + (compact ? 14 : 10)))
    color: Theme.editor
    FontMetrics { id: addressMetrics; font.family: Theme.monoFont; font.pointSize: Theme.bodySize }
    function focusContent() { references.forceActiveFocus(Qt.OtherFocusReason) }
    ListView {
        id: references
        objectName: "referencesList"
        anchors.fill: parent
        clip: true
        reuseItems: true
        model: root.controller.xrefsModel
        activeFocusOnTab: true
        keyNavigationEnabled: false
        boundsBehavior: Flickable.StopAtBounds
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        function moveCurrent(index) {
            if (count === 0) return
            currentIndex = Math.max(0, Math.min(count - 1, index))
            positionViewAtIndex(currentIndex, ListView.Contain)
        }
        function activateCurrent() {
            const row = root.controller.xrefsModel.get(currentIndex)
            if (row.from) root.controller.navigate(row.from)
        }
        Keys.onPressed: event => {
            if (event.matches(StandardKey.Copy)) {
                const row = root.controller.xrefsModel.get(currentIndex)
                if (row.from) root.controller.copyText(row.from + " " + row.kind + " " + row.to)
                event.accepted = true
                return
            }
            if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) return
            const page = Math.max(1, Math.floor(height / root.referenceRowHeight) - 1)
            switch (event.key) {
            case Qt.Key_Up: moveCurrent(currentIndex - 1); break
            case Qt.Key_Down: moveCurrent(currentIndex + 1); break
            case Qt.Key_Home: moveCurrent(0); break
            case Qt.Key_End: moveCurrent(count - 1); break
            case Qt.Key_PageUp: moveCurrent(currentIndex - page); break
            case Qt.Key_PageDown: moveCurrent(currentIndex + page); break
            case Qt.Key_Return:
            case Qt.Key_Enter: activateCurrent(); break
            default: return
            }
            event.accepted = true
        }
        delegate: ItemDelegate {
            id: reference
            required property int index
            required property string from
            required property string to
            required property string kind
            width: references.width
            height: root.referenceRowHeight
            topPadding: 4
            bottomPadding: 4
            leftPadding: 14
            rightPadding: 14
            hoverEnabled: true
            focusPolicy: Qt.NoFocus
            highlighted: references.activeFocus && ListView.isCurrentItem
            contentItem: GridLayout {
                columns: root.compact ? 3 : 4
                columnSpacing: 12
                rowSpacing: 4
                Text {
                    textFormat: Text.PlainText
                    text: reference.kind
                    color: Theme.muted
                    font.pointSize: Theme.captionSize
                    elide: Text.ElideRight
                    Layout.row: 0
                    Layout.column: root.compact ? 2 : 0
                    Layout.preferredWidth: root.compact ? 60 : 100
                    Layout.maximumWidth: root.compact ? 60 : 100
                }
                Text {
                    textFormat: Text.PlainText
                    text: reference.from
                    color: Theme.variable
                    font: addressMetrics.font
                    elide: Text.ElideRight
                    Layout.row: 0
                    Layout.column: root.compact ? 0 : 1
                    Layout.columnSpan: root.compact ? 2 : 1
                    Layout.preferredWidth: root.addressWidth
                    Layout.fillWidth: true
                }
                Text {
                    textFormat: Text.PlainText
                    text: "→"
                    color: Theme.subdued
                    font.pointSize: Theme.bodySize
                    Layout.row: root.compact ? 1 : 0
                    Layout.column: root.compact ? 0 : 2
                }
                Text {
                    textFormat: Text.PlainText
                    text: reference.to
                    color: Theme.functionName
                    font: addressMetrics.font
                    elide: Text.ElideRight
                    Layout.row: root.compact ? 1 : 0
                    Layout.column: root.compact ? 1 : 3
                    Layout.columnSpan: root.compact ? 2 : 1
                    Layout.preferredWidth: root.addressWidth
                    Layout.fillWidth: true
                }
            }
            background: Rectangle {
                color: reference.highlighted ? Theme.selection : reference.ListView.isCurrentItem ? Theme.inactiveSelection : reference.hovered ? Theme.hover : "transparent"
                border.color: reference.highlighted ? Theme.focus : "transparent"
            }
            onClicked: {
                references.currentIndex = index
                references.forceActiveFocus(Qt.MouseFocusReason)
                root.controller.navigate(from)
            }
            onDoubleClicked: {}
            Accessible.name: from + " " + kind + " " + to
            ToolTip.text: from + " " + kind + " " + to
            ToolTip.visible: hovered
            ToolTip.delay: 900
        }
    }
    Text { textFormat: Text.PlainText; anchors.centerIn: parent; visible: references.count === 0; text: root.controller.loaded ? qsTranslate("Main", "No references for this selection.") : qsTranslate("Main", "References to the selected address appear here."); color: Theme.subdued; font.pointSize: Theme.bodySize; width: parent.width - 36; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter }
}
