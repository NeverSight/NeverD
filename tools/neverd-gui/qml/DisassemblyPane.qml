pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property var controller
    property real codePointSize: Theme.codeSize
    readonly property string selectedInstruction: controller.selectedAddress
    color: Theme.editor
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            Layout.leftMargin: 14
            spacing: 14
            Text { textFormat: Text.PlainText; text: qsTr("Address"); color: Theme.subdued; font.pointSize: Theme.captionSize; Layout.preferredWidth: 115 }
            Text { textFormat: Text.PlainText; text: qsTr("Instruction"); color: Theme.subdued; font.pointSize: Theme.captionSize; Layout.fillWidth: true }
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
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: ItemDelegate {
                id: instruction
                required property int index
                required property string address
                required property string bytes
                required property string mnemonic
                required property string operands
                required property string comment
                width: listing.width
                height: Math.ceil(root.codePointSize * 2.1)
                leftPadding: 14
                rightPadding: 14
                hoverEnabled: true
                text: address + " " + mnemonic + " " + operands
                Accessible.name: text
                contentItem: RowLayout {
                    spacing: 14
                    Text { textFormat: Text.PlainText; text: instruction.address.replace(/^0x/, ""); font.family: Theme.monoFont; font.pointSize: root.codePointSize - 1; color: Theme.subdued; Layout.preferredWidth: 115 }
                    Text { textFormat: Text.PlainText;
                        text: instruction.mnemonic
                        font.family: Theme.monoFont
                        font.pointSize: root.codePointSize
                        color: /^(j|b\.|call|ret|cb|tb)/i.test(text) ? Theme.controlFlow : Theme.keyword
                        Layout.preferredWidth: 62
                    }
                    Text { textFormat: Text.PlainText; text: instruction.operands; font.family: Theme.monoFont; font.pointSize: root.codePointSize; color: Theme.foreground; elide: Text.ElideRight; Layout.fillWidth: true }
                    Text { textFormat: Text.PlainText; text: instruction.comment ? "; " + instruction.comment : ""; visible: text.length > 0 && root.width > 640; font.family: Theme.monoFont; font.pointSize: root.codePointSize; color: Theme.comment; elide: Text.ElideRight; Layout.maximumWidth: root.width * 0.3 }
                }
                background: Rectangle {
                    color: root.selectedInstruction === instruction.address ? Theme.selection : instruction.hovered ? Theme.hover : "transparent"
                    border.color: instruction.activeFocus ? Theme.focus : "transparent"
                    Rectangle { visible: root.selectedInstruction === instruction.address; width: 2; height: parent.height; color: Theme.accent }
                }
                onClicked: {
                    listing.currentIndex = index
                    root.controller.selectInstruction(address)
                }
                onDoubleClicked: root.controller.navigate(address)
                Keys.onPressed: event => {
                    if (event.matches(StandardKey.Copy)) {
                        root.controller.copyText(address + "  " + mnemonic + " " + operands + (comment ? " ; " + comment : ""))
                        event.accepted = true
                    }
                }
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
