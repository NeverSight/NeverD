pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property var controller
    property bool showTitle: true
    signal renameRequested()
    signal commentRequested()
    color: Theme.sidebar
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
            onTextEdited: searchDebounce.restart()
            Keys.onDownPressed: functions.forceActiveFocus()
        }
        Timer {
            id: searchDebounce
            interval: 180
            onTriggered: root.controller.filterFunctions(filter.text)
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
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: ItemDelegate {
                id: functionRow
                required property string name
                required property string address
                required property var size
                required property int index
                width: functions.width
                height: 29
                hoverEnabled: true
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
                        color: Theme.subdued
                        LayoutMirroring.enabled: false
                    }
                }
                leftPadding: 13
                rightPadding: 12
                background: Rectangle {
                    color: functionRow.address === root.controller.selectedFunctionAddress ? Theme.selection : functionRow.hovered ? Theme.hover : "transparent"
                    border.color: functionRow.activeFocus ? Theme.focus : "transparent"
                }
                onClicked: {
                    functions.currentIndex = index
                    root.controller.selectFunction(address)
                }
                onDoubleClicked: root.controller.selectFunction(address)
                ToolTip {
                    visible: functionRow.hovered && functionRow.name.length > 24
                    text: functionRow.name + "  " + functionRow.address
                    delay: 700
                    width: 440
                    contentItem: Text { textFormat: Text.PlainText; text: functionRow.name + "  " + functionRow.address; color: Theme.foreground; font.pointSize: Theme.captionSize; wrapMode: Text.WordWrap; maximumLineCount: 8; elide: Text.ElideRight }
                }
            }
            footer: WorkbenchButton {
                width: functions.width
                visible: functions.count > 0
                text: qsTr("Load more functions")
                onClicked: root.controller.loadMoreFunctions()
            }
            Keys.onReturnPressed: {
                const row = currentItem as ItemDelegate
                if (row) row.clicked()
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
                WorkbenchButton { text: qsTr("Rename"); enabled: root.controller.loaded; onClicked: root.renameRequested() }
                WorkbenchButton { text: qsTr("Comment"); enabled: root.controller.loaded; onClicked: root.commentRequested() }
            }
        }
    }
}
