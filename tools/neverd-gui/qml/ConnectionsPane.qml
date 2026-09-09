pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    required property var client
    required property var broker
    required property var controller
    property string validationError: ""
    property var selectedTool: null
    property string selectedCallId: ""
    readonly property var selectedCall: {
        const history = root.client.callHistory
        return history.find(call => call.id === root.selectedCallId) || null
    }
    function callStatus(status) {
        switch (status) {
        case "pending": return qsTr("Pending")
        case "completed": return qsTr("Completed")
        case "failed": return qsTr("Failed")
        case "cancelled": return qsTr("Cancelled")
        case "timed_out": return qsTr("Timed out")
        case "disconnected": return qsTr("Disconnected")
        default: return status
        }
    }
    RowLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 24
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            Text { textFormat: Text.PlainText; text: qsTr("MCP connections"); color: Theme.foreground; font.pointSize: Theme.bodySize; font.weight: Font.Medium }
            Text { textFormat: Text.PlainText; text: root.client.status || qsTr("No server connected"); color: Theme.muted; font.pointSize: Theme.bodySize; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Text { textFormat: Text.PlainText; text: root.broker.enabled ? qsTr("Current session is available to external agents.") : qsTr("Connect tools and resources, or share this session with an external agent."); color: Theme.subdued; font.pointSize: Theme.bodySize; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Item { Layout.fillHeight: true }
        }
        WorkbenchButton { text: qsTr("Manage Connections…"); onClicked: manager.open() }
    }
    Dialog {
        id: manager
        objectName: "connectionManager"
        title: qsTr("MCP Connections")
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(1080, parent.width - 50)
        height: Math.min(760, parent.height - 50)
        modal: true
        standardButtons: Dialog.Close
        background: Rectangle { color: Theme.sidebar; border.color: Theme.border; radius: 4 }
        ColumnLayout {
            anchors.fill: parent
            spacing: 14
            RowLayout {
                Layout.fillWidth: true
                Text { textFormat: Text.PlainText; text: qsTr("Current GUI session"); color: Theme.foreground; font.pointSize: Theme.bodySize; Layout.fillWidth: true }
                WorkbenchButton { text: root.broker.enabled ? qsTr("Disable Sharing") : qsTr("Enable Sharing"); enabled: root.controller.loaded; onClicked: root.broker.enabled ? root.broker.stop() : root.broker.start() }
                WorkbenchButton { text: qsTr("Copy Credential Path"); enabled: root.broker.enabled; onClicked: root.controller.copyText(root.broker.credentialFile) }
            }
            Text { textFormat: Text.PlainText; text: root.broker.status || qsTr("Sharing is off. Enabling it grants local agents access to this session using its private credential file."); color: Theme.muted; font.pointSize: Theme.captionSize; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
            RowLayout {
                Layout.fillWidth: true
                ComboBox { id: transport; model: ["stdio", "Streamable HTTP"]; Layout.preferredWidth: 175; enabled: !root.client.connected; Accessible.name: qsTr("Transport") }
                WorkbenchField { id: endpoint; Layout.fillWidth: true; placeholderText: transport.currentIndex === 0 ? qsTr("Absolute path to server executable") : "https://localhost:8443/mcp"; enabled: !root.client.connected; LayoutMirroring.enabled: false; Accessible.name: qsTr("Server endpoint") }
                WorkbenchButton {
                    text: root.client.connected ? qsTr("Disconnect") : qsTr("Connect")
                    primary: !root.client.connected
                    onClicked: {
                        if (root.client.connected) { root.client.disconnectServer(); token.clear(); return }
                        root.validationError = ""
                        if (transport.currentIndex === 0) {
                            try {
                                const args = JSON.parse(argumentsField.text || "[]")
                                if (!Array.isArray(args) || !args.every(value => typeof value === "string")) throw new Error("arguments")
                                root.client.connectStdio(endpoint.text, args)
                            } catch (error) { root.validationError = qsTr("Arguments must be a JSON array of strings.") }
                        } else {
                            root.client.connectHttp(endpoint.text, token.text, caFile.text)
                        }
                    }
                }
            }
            WorkbenchField { id: argumentsField; Layout.fillWidth: true; visible: transport.currentIndex === 0; placeholderText: qsTr("Arguments as JSON, for example [\"--help\"]"); text: "[]"; font.family: Theme.monoFont; LayoutMirroring.enabled: false; enabled: !root.client.connected; Accessible.name: qsTr("Server arguments") }
            RowLayout {
                visible: transport.currentIndex === 1
                Layout.fillWidth: true
                WorkbenchField { id: token; Layout.fillWidth: true; placeholderText: qsTr("Bearer token (optional)"); echoMode: TextInput.Password; enabled: !root.client.connected; Accessible.name: qsTr("Bearer token") }
                WorkbenchField { id: caFile; Layout.fillWidth: true; placeholderText: qsTr("CA certificate path (optional)"); enabled: !root.client.connected; LayoutMirroring.enabled: false; Accessible.name: qsTr("CA certificate path") }
            }
            Text { textFormat: Text.PlainText; text: root.validationError || root.client.status; color: root.validationError ? Theme.error : Theme.muted; font.pointSize: Theme.captionSize; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            SplitView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                handle: Rectangle { implicitWidth: 4; color: SplitHandle.hovered || SplitHandle.pressed ? Theme.accent : Theme.border }
                ColumnLayout {
                    SplitView.preferredWidth: 310
                    SplitView.minimumWidth: 220
                    spacing: 0
                    PanelTabs {
                        id: catalogTabs
                        Layout.fillWidth: true
                        labels: [qsTr("Tools"), qsTr("Resources"), qsTr("History")]
                        onSelected: index => {
                            currentIndex = index
                            root.selectedCallId = ""
                            root.selectedTool = null
                            if (root.client.connected && index < 2) index === 0 ? root.client.listTools() : root.client.listResources()
                        }
                    }
                    ListView {
                        id: catalog
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        reuseItems: true
                        model: catalogTabs.currentIndex === 0 ? root.client.tools : catalogTabs.currentIndex === 1 ? root.client.resources : root.client.callHistory
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ItemDelegate {
                            id: catalogRow
                            required property var modelData
                            width: catalog.width
                            height: 44
                            leftPadding: 12
                            rightPadding: 12
                            hoverEnabled: true
                            contentItem: Column {
                                Text { textFormat: Text.PlainText; width: parent.width; text: catalogRow.modelData.name || catalogRow.modelData.uri || catalogRow.modelData.method; color: Theme.foreground; font.pointSize: Theme.bodySize; elide: Text.ElideRight }
                                Text { textFormat: Text.PlainText; visible: catalogTabs.currentIndex === 2; width: parent.width; text: root.callStatus(catalogRow.modelData.status || ""); color: catalogRow.modelData.status === "failed" || catalogRow.modelData.status === "timed_out" ? Theme.error : Theme.subdued; font.pointSize: Theme.captionSize; elide: Text.ElideRight }
                            }
                            background: Rectangle { color: (catalogTabs.currentIndex === 2 ? root.selectedCallId === catalogRow.modelData.id : root.selectedTool && root.selectedTool.name === catalogRow.modelData.name) ? Theme.selection : catalogRow.hovered ? Theme.hover : Theme.editor; border.color: catalogRow.activeFocus ? Theme.focus : "transparent" }
                            onClicked: {
                                root.selectedCallId = ""
                                if (catalogTabs.currentIndex === 0) { root.selectedTool = modelData; parameters.text = "{}" }
                                else if (catalogTabs.currentIndex === 1) { root.selectedTool = null; root.client.readResource(modelData.uri) }
                                else { root.selectedTool = null; root.selectedCallId = modelData.id; root.client.inspectCall(modelData.id) }
                            }
                            Accessible.name: (modelData.name || modelData.uri || modelData.method) + (catalogTabs.currentIndex === 2 ? ": " + root.callStatus(modelData.status) : "")
                            ToolTip {
                                visible: catalogRow.hovered && text.length > 0
                                text: catalogRow.modelData.description || ""
                                delay: 700
                                width: Math.min(480, manager.width - 40)
                                contentItem: Text { textFormat: Text.PlainText; text: catalogRow.modelData.description || ""; color: Theme.foreground; font.pointSize: Theme.captionSize; wrapMode: Text.WordWrap; maximumLineCount: 8; elide: Text.ElideRight }
                            }
                        }
                        EmptyPane { anchors.fill: parent; visible: catalog.count === 0; title: ""; detail: catalogTabs.currentIndex === 2 ? qsTr("No calls yet.") : root.client.connected ? qsTr("No items published by this server.") : qsTr("Connect a server to browse its tools and resources.") }
                    }
                }
                ColumnLayout {
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 320
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 10
                        Text { textFormat: Text.PlainText; text: root.selectedTool ? root.selectedTool.name : qsTr("Result"); color: Theme.foreground; font.pointSize: Theme.bodySize; elide: Text.ElideRight; Layout.fillWidth: true }
                        WorkbenchButton { text: qsTr("Schema"); visible: catalogTabs.currentIndex !== 2; enabled: !!root.selectedTool; onClicked: schemaDialog.open() }
                        WorkbenchButton { text: qsTr("Call Tool"); visible: catalogTabs.currentIndex !== 2; enabled: !!root.selectedTool && root.client.connected; onClicked: root.client.callTool(root.selectedTool.name, parameters.text) }
                        WorkbenchButton { text: qsTr("Cancel Call"); visible: catalogTabs.currentIndex === 2; enabled: !!root.selectedCall && root.selectedCall.status === "pending"; onClicked: root.client.cancelCall(root.selectedCallId) }
                    }
                    Text { textFormat: Text.PlainText; visible: !!root.selectedTool; text: root.selectedTool ? root.selectedTool.description || "" : ""; color: Theme.muted; font.pointSize: Theme.captionSize; wrapMode: Text.WordWrap; Layout.fillWidth: true; Layout.leftMargin: 10; maximumLineCount: 3; elide: Text.ElideRight }
                    ScrollView {
                        visible: !!root.selectedTool
                        Layout.fillWidth: true
                        Layout.preferredHeight: 94
                        Layout.leftMargin: 10
                        TextArea { id: parameters; text: "{}"; color: Theme.foreground; font.family: Theme.monoFont; font.pointSize: Theme.bodySize; selectByMouse: true; wrapMode: TextEdit.Wrap; background: Rectangle { color: Theme.editor; border.color: parameters.activeFocus ? Theme.focus : Theme.border } LayoutMirroring.enabled: false; Accessible.name: qsTr("Tool arguments as JSON") }
                    }
                    TextPane { Layout.fillWidth: true; Layout.fillHeight: true; text: root.client.lastResult; emptyTitle: ""; emptyDetail: qsTr("Select a resource or call a tool to inspect its response."); codePointSize: Theme.bodySize }
                }
            }
        }
        onClosed: token.clear()
    }
    Dialog {
        id: schemaDialog
        title: qsTr("Tool Input Schema")
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(680, parent.width - 80)
        height: Math.min(560, parent.height - 80)
        modal: true
        standardButtons: Dialog.Close
        background: Rectangle { color: Theme.sidebar; border.color: Theme.border; radius: 4 }
        TextPane { anchors.fill: parent; text: root.selectedTool ? JSON.stringify(root.selectedTool.inputSchema, null, 2) : ""; codePointSize: Theme.bodySize }
    }
}
