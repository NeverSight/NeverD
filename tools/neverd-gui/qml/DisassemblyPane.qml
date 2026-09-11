pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property var controller
    property real codePointSize: Theme.codeSize
    readonly property string selectedInstruction: controller.selectedAddress
    property bool selectionPending: true
    readonly property real addressColumnWidth: Math.ceil(addressMetrics.advanceWidth)
    readonly property real mnemonicColumnWidth: Math.ceil(codeMetrics.averageCharacterWidth * 8)
    readonly property real instructionRowHeight: Math.ceil(codeMetrics.height + 4)
    readonly property real minimumListingWidth: addressColumnWidth + mnemonicColumnWidth + Math.ceil(codeMetrics.averageCharacterWidth * 24) + Theme.codeGutter * 4
    color: Theme.editor
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    TextMetrics { id: addressMetrics; text: "0000000000000000"; font.family: Theme.monoFont; font.pointSize: root.codePointSize }
    FontMetrics { id: codeMetrics; font.family: Theme.monoFont; font.pointSize: root.codePointSize }
    function focusContent() { listing.forceActiveFocus(Qt.OtherFocusReason) }
    function revealSelection() {
        if (!selectionPending) return
        const index = controller.instructionsModel.findRow("address", selectedInstruction)
        if (index >= 0) {
            selectionPending = false
            listing.currentIndex = index
            listing.positionViewAtIndex(index, ListView.Contain)
        }
    }
    onSelectedInstructionChanged: { selectionPending = true; Qt.callLater(revealSelection) }
    Component.onCompleted: Qt.callLater(revealSelection)
    Connections {
        target: root.controller.instructionsModel
        function onModelReset() { root.selectionPending = true; Qt.callLater(root.revealSelection) }
        function onCountChanged() { if (root.selectionPending) Qt.callLater(root.revealSelection) }
        function onDataChanged() { if (root.selectionPending) Qt.callLater(root.revealSelection) }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.compactControlHeight
            clip: true
            RowLayout {
                x: Theme.codeGutter - listing.contentX
                width: listing.contentWidth - Theme.codeGutter * 2
                height: parent.height
                spacing: Theme.codeGutter
                Text { textFormat: Text.PlainText; text: qsTr("Address"); color: Theme.subdued; font.pointSize: Theme.captionSize; Layout.preferredWidth: root.addressColumnWidth }
                Text { textFormat: Text.PlainText; text: qsTr("Instruction"); color: Theme.subdued; font.pointSize: Theme.captionSize; Layout.fillWidth: true }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        ListView {
            id: listing
            objectName: "disassemblyList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: root.controller.instructionsModel
            clip: true
            reuseItems: true
            activeFocusOnTab: true
            keyNavigationEnabled: false
            contentWidth: Math.max(width, root.minimumListingWidth)
            flickableDirection: Flickable.AutoFlickIfNeeded
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: WorkbenchScrollBar { policy: ScrollBar.AsNeeded }
            ScrollBar.horizontal: WorkbenchScrollBar { policy: ScrollBar.AsNeeded }
            function moveCurrent(index) {
                if (count === 0) return
                currentIndex = Math.max(0, Math.min(count - 1, index))
                positionViewAtIndex(currentIndex, ListView.Contain)
                const row = root.controller.instructionsModel.get(currentIndex)
                if (row.address) root.controller.selectInstruction(row.address)
            }
            function activateCurrent() {
                const row = root.controller.instructionsModel.get(currentIndex)
                if (row.address) root.controller.navigate(row.address)
            }
            function copyCurrent() {
                const row = root.controller.instructionsModel.get(currentIndex)
                if (row.address) root.controller.copyText(row.address + "  " + row.mnemonic + " " + row.operands + (row.comment ? " ; " + row.comment : ""))
            }
            Keys.onPressed: event => {
                if (event.matches(StandardKey.Copy)) {
                    copyCurrent()
                    event.accepted = true
                    return
                }
                if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) return
                const page = Math.max(1, Math.floor(height / root.instructionRowHeight) - 1)
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
                id: instruction
                objectName: "instructionRow"
                required property int index
                required property string address
                required property string bytes
                required property string mnemonic
                required property string operands
                required property string comment
                width: listing.contentWidth
                height: root.instructionRowHeight
                topPadding: 2
                bottomPadding: 2
                leftPadding: Theme.codeGutter
                rightPadding: Theme.codeGutter
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                highlighted: listing.activeFocus && ListView.isCurrentItem
                text: address + " " + mnemonic + " " + operands
                Accessible.name: text
                contentItem: RowLayout {
                    spacing: Theme.codeGutter
                    Text { textFormat: Text.PlainText; text: instruction.address.replace(/^0x/, ""); font: addressMetrics.font; color: instruction.highlighted || root.selectedInstruction === instruction.address || instruction.hovered ? Theme.muted : Theme.subdued; Layout.preferredWidth: root.addressColumnWidth }
                    Text { textFormat: Text.PlainText;
                        text: instruction.mnemonic
                        font: codeMetrics.font
                        color: /^(j|b\.|call|ret|cb|tb)/i.test(text) ? Theme.controlFlow : Theme.keyword
                        Layout.preferredWidth: root.mnemonicColumnWidth
                        elide: Text.ElideRight
                    }
                    Text { textFormat: Text.PlainText; text: instruction.operands; font: codeMetrics.font; color: Theme.foreground; elide: Text.ElideRight; Layout.fillWidth: true }
                    Text { textFormat: Text.PlainText; text: instruction.comment ? "; " + instruction.comment : ""; visible: text.length > 0 && root.width > 640; font: codeMetrics.font; color: Theme.comment; elide: Text.ElideRight; Layout.maximumWidth: root.width * 0.3 }
                }
                background: Rectangle {
                    color: instruction.highlighted ? Theme.selection : root.selectedInstruction === instruction.address ? Theme.inactiveSelection : instruction.hovered ? Theme.hover : "transparent"
                    border.color: instruction.highlighted ? Theme.focus : "transparent"
                    Rectangle { visible: root.selectedInstruction === instruction.address; width: 2; height: parent.height; color: Theme.accent }
                }
                onClicked: {
                    listing.currentIndex = index
                    listing.forceActiveFocus(Qt.MouseFocusReason)
                    root.controller.selectInstruction(address)
                }
                onDoubleClicked: root.controller.navigate(address)
                ToolTip.text: address + "  " + bytes + "\n" + mnemonic + " " + operands + (comment ? "\n" + comment : "")
                ToolTip.visible: hovered
                ToolTip.delay: 900
            }
            footer: WorkbenchButton {
                width: listing.width
                visible: listing.count > 0
                text: qsTr("Load more instructions")
                onClicked: root.controller.loadMoreInstructions()
            }
            EmptyPane {
                anchors.fill: parent
                visible: listing.count === 0
                title: qsTr("Disassembly")
                detail: root.controller.loaded ? qsTr("Select a function to inspect its instructions.") : qsTr("Addresses, instructions, and control flow in one place.")
            }
        }
    }
}
