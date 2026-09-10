pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property var controller
    property bool showTitle: true
    property bool canRename: controller.loaded && controller.selectedFunctionAddress.length > 0
    property bool canComment: controller.loaded && controller.selectedAddress.length > 0
    readonly property string selectedFunction: controller.selectedFunctionAddress
    property bool selectionPending: true
    signal renameRequested()
    signal commentRequested()
    signal navigationRequested(string address)
    color: Theme.sidebar
    function focusContent() { functions.forceActiveFocus(Qt.OtherFocusReason) }
    function focusSearch() { filter.forceActiveFocus(Qt.ShortcutFocusReason); filter.selectAll() }
    function revealSelection() {
        if (!selectionPending) return
        const index = controller.functionsModel.findRow("address", selectedFunction)
        if (index >= 0) {
            selectionPending = false
            functions.currentIndex = index
            functions.positionViewAtIndex(index, ListView.Contain)
        }
    }
    onSelectedFunctionChanged: { selectionPending = true; Qt.callLater(revealSelection) }
    Component.onCompleted: Qt.callLater(revealSelection)
    Connections {
        target: root.controller.functionsModel
        function onCountChanged() { root.selectionPending = true; Qt.callLater(root.revealSelection) }
        function onDataChanged() { if (root.selectionPending) Qt.callLater(root.revealSelection) }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            visible: root.showTitle
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            spacing: 6
            Text { textFormat: Text.PlainText;
                text: qsTr("FUNCTIONS")
                font.pointSize: Theme.captionSize
                font.letterSpacing: 1
                color: Theme.muted
                Layout.leftMargin: 14
            }
            Item { Layout.fillWidth: true }
            Text { textFormat: Text.PlainText;
                text: Number(root.controller.functionCount).toLocaleString(Qt.locale(), "f", 0)
                color: Theme.subdued
                font.pointSize: Theme.captionSize
                Layout.rightMargin: 14
            }
        }
        WorkbenchField {
            id: filter
            objectName: "functionSearch"
            Layout.fillWidth: true
            Layout.margins: 10
            Layout.topMargin: 4
            placeholderText: qsTr("Filter functions…")
            Accessible.name: qsTr("Filter functions")
            onTextEdited: root.controller.filterFunctions(text)
            Keys.onDownPressed: functions.forceActiveFocus()
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            Layout.leftMargin: 14
            Layout.rightMargin: 12
            Text { textFormat: Text.PlainText; text: qsTr("Name"); color: Theme.subdued; font.pointSize: Theme.captionSize; Layout.fillWidth: true }
            Text { textFormat: Text.PlainText; text: qsTr("Address"); color: Theme.subdued; font.pointSize: Theme.captionSize }
        }
        ListView {
            id: functions
            objectName: "functionsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.controller.functionsModel
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            activeFocusOnTab: true
            keyNavigationEnabled: false
            function moveCurrent(index) {
                if (count === 0) return
                currentIndex = Math.max(0, Math.min(count - 1, index))
                positionViewAtIndex(currentIndex, ListView.Contain)
            }
            function activateCurrentFunction() {
                const row = root.controller.functionsModel.get(currentIndex)
                if (row.address) root.navigationRequested(row.address)
            }
            Keys.onPressed: event => {
                if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) return
                const page = Math.max(1, Math.floor(height / Theme.rowHeight) - 1)
                switch (event.key) {
                case Qt.Key_Up: moveCurrent(currentIndex - 1); break
                case Qt.Key_Down: moveCurrent(currentIndex + 1); break
                case Qt.Key_Home: moveCurrent(0); break
                case Qt.Key_End: moveCurrent(count - 1); break
                case Qt.Key_PageUp: moveCurrent(currentIndex - page); break
                case Qt.Key_PageDown: moveCurrent(currentIndex + page); break
                case Qt.Key_Return:
                case Qt.Key_Enter: activateCurrentFunction(); break
                default: return
                }
                event.accepted = true
            }
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: ItemDelegate {
                id: functionRow
                required property string name
                required property string address
                required property var size
                required property int index
                width: functions.width
                height: Theme.rowHeight
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                highlighted: functions.activeFocus && ListView.isCurrentItem
                text: name + " " + address
                Accessible.name: text
                contentItem: RowLayout {
                    spacing: 9
                    Text { textFormat: Text.PlainText; text: "ƒ"; font.pointSize: 12; color: Theme.functionName }
                    Text { textFormat: Text.PlainText;
                        text: functionRow.name
                        color: Theme.foreground
                        font.pointSize: Theme.bodySize
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text { textFormat: Text.PlainText;
                        text: functionRow.address.replace(/^0x/, "")
                        font.family: Theme.monoFont
                        font.pointSize: Theme.captionSize
                        color: functionRow.highlighted || functionRow.address === root.selectedFunction || functionRow.hovered ? Theme.muted : Theme.subdued
                        LayoutMirroring.enabled: false
                    }
                }
                topPadding: 4
                bottomPadding: 4
                leftPadding: 13
                rightPadding: 12
                background: Rectangle {
                    color: functionRow.highlighted ? Theme.selection : functionRow.address === root.selectedFunction ? Theme.inactiveSelection : functionRow.hovered ? Theme.hover : "transparent"
                    border.color: functionRow.highlighted ? Theme.focus : "transparent"
                    Rectangle { visible: functionRow.address === root.selectedFunction; width: 2; height: parent.height; color: Theme.accent }
                }
                onClicked: {
                    functions.currentIndex = index
                    functions.forceActiveFocus(Qt.MouseFocusReason)
                    root.navigationRequested(address)
                }
                // Consume the second click; the first already navigated.
                // Without a handler, AbstractButton emits clicked again.
                onDoubleClicked: {}
                ToolTip {
                    visible: (functionRow.hovered || functionRow.highlighted) && functionRow.name.length > 24
                    text: functionRow.name + "  " + functionRow.address
                    delay: 700
                    width: 440
                    contentItem: Text { textFormat: Text.PlainText; text: functionRow.name + "  " + functionRow.address; color: Theme.foreground; font.pointSize: Theme.captionSize; wrapMode: Text.WordWrap; maximumLineCount: 8; elide: Text.ElideRight }
                }
            }
            EmptyPane {
                anchors.fill: parent
                visible: functions.count === 0
                title: root.controller.loaded ? qsTr("No functions") : ""
                detail: root.controller.loaded ? qsTr("Try another filter or wait for analysis.") : qsTr("Open a binary to browse its functions.")
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            spacing: 8
            Text { textFormat: Text.PlainText; text: qsTr("CURRENT FUNCTION"); font.pointSize: Theme.captionSize; font.letterSpacing: 1; color: Theme.subdued }
            Text { textFormat: Text.PlainText;
                text: root.controller.selectedFunctionName || "—"
                font.family: Theme.monoFont
                font.pointSize: Theme.bodySize
                color: Theme.functionName
                elide: Text.ElideRight
                Layout.fillWidth: true
                LayoutMirroring.enabled: false
            }
            Text { textFormat: Text.PlainText;
                text: root.controller.selectedAddress || "—"
                font.family: Theme.monoFont
                font.pointSize: Theme.captionSize
                color: Theme.muted
                LayoutMirroring.enabled: false
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 3
                WorkbenchButton { text: qsTr("Rename"); enabled: root.canRename; onClicked: root.renameRequested() }
                WorkbenchButton { text: qsTr("Comment"); enabled: root.canComment; onClicked: root.commentRequested() }
            }
        }
    }
}
