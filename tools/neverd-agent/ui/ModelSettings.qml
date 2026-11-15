import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

Dialog {
    id: dialog
    required property var backend
    property var protocols: ["responses", "messages", "chat-completions"]
    property var efforts: ["", "low", "medium", "high", "max"]
    property var baseline: ({})
    modal: true
    anchors.centerIn: parent
    Overlay.modal: ModalDim {}
    width: Math.min(640, parent.width - 48)
    height: Math.min(implicitHeight, parent.height - 48)
    padding: 18
    topPadding: 4
    background: Surface { glass: true; radius: Theme.radius; color: Theme.background }
    header: Label { text: qsTr("Model and analysis settings"); color: Theme.text; font.pointSize: Theme.bodyPointSize; font.weight: Font.Medium; padding: 18 }
    function values() {
        return {protocol: protocols[protocol.currentIndex], url: endpoint.text.trim(), model: modelName.text.trim(),
            contextTokens: Number(contextSize.text), outputTokens: Number(outputSize.text),
            timeoutSeconds: Number(timeout.text), maxAgents: Number(agents.text),
            maxDepth: Number(depth.text), anthropicVersion: version.text,
            completionTokenField: tokenField.currentIndex === 0 ? "max_completion_tokens" : "max_tokens",
            reasoningEffort: efforts[effort.currentIndex]}
    }
    // The CLI's saved settings; empty while the CLI is unavailable, when
    // every field starts blank instead of failing.
    function load() {
        baseline = backend.modelSettings || ({})
        const text = value => value === undefined || value === null ? "" : String(value)
        protocol.currentIndex = Math.max(0, protocols.indexOf(baseline.protocol))
        endpoint.text = text(baseline.url); modelName.text = text(baseline.model)
        contextSize.text = text(baseline.contextTokens); outputSize.text = text(baseline.outputTokens)
        timeout.text = text(baseline.timeoutSeconds)
        agents.text = text(baseline.maxAgents); depth.text = text(baseline.maxDepth)
        version.text = text(baseline.anthropicVersion)
        tokenField.currentIndex = baseline.completionTokenField === "max_tokens" ? 1 : 0
        effort.currentIndex = Math.max(0, efforts.indexOf(baseline.reasoningEffort || ""))
        mcp.text = text(backend.mcpPath); secret.clear(); removeKey.checked = false; reveal.checked = false
    }
    readonly property bool hasSavedKey: !!(baseline && baseline.hasKey)
    onOpened: load()
    onClosed: { secret.clear(); backend.cancelConnectionTest() }
    Connections {
        target: dialog.backend
        function onModelSettingsSaved() { if (dialog.visible) dialog.close() }
    }
    contentItem: ScrollView {
        clip: true
        implicitHeight: contentHeight
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: 8
            Label { text: qsTr("API protocol"); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            ChoiceField { id: protocol; model: ["OpenAI Responses", "Anthropic Messages", "OpenAI Chat Completions"]; Layout.fillWidth: true }
            Label { text: qsTr("API URL"); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.topMargin: 8 }
            Field {
                id: endpoint; Layout.fillWidth: true
                placeholderText: protocol.currentIndex === 1 ? "https://api.anthropic.com/v1" : "https://api.openai.com/v1"
            }
            Label { text: qsTr("Enter an API base URL or a full endpoint URL. Self-hosted services can use HTTP."); color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            Label { text: qsTr("API key"); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.topMargin: 8 }
            Field {
                id: secret
                Layout.fillWidth: true
                echoMode: reveal.checked ? TextInput.Normal : TextInput.Password
                placeholderText: dialog.hasSavedKey ? qsTr("Saved · Leave empty to keep the current key") : qsTr("Enter an API key, or leave empty for a local service without authentication")
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            }
            RowLayout {
                spacing: 16
                CheckField { id: reveal; text: qsTr("Show new key") }
                CheckField { id: removeKey; visible: dialog.hasSavedKey; text: qsTr("Remove saved key") }
            }
            Label { text: qsTr("Model name"); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.topMargin: 8 }
            Field { id: modelName; Layout.fillWidth: true; placeholderText: qsTr("Model ID used by the server") }
            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                columns: 2; columnSpacing: 16; rowSpacing: 8
                Label { text: qsTr("Context window · tokens"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("Output limit per response · tokens"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: contextSize; Layout.fillWidth: true; validator: IntValidator { bottom: 1 } }
                Field { id: outputSize; Layout.fillWidth: true; validator: IntValidator { bottom: 1 } }
            }
            Label {
                text: qsTr("Output space is reserved, and context is compacted automatically at 80% of the available input allowance. Compaction keeps the current question, key evidence and the latest complete calls; the original records stay in the project. Server-reported usage calibrates the local estimate.")
                color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true
            }
            Label { text: qsTr("NeverD MCP server executable"); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.topMargin: 8 }
            Field { id: mcp; Layout.fillWidth: true; placeholderText: qsTr("neverd-mcp, or leave empty to search the adjacent directory, the NeverD workbench directory and PATH") }
            Label { text: qsTr("Analysis uses NeverD tools. Each object runs in its own process, and files are opened read-only."); color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            CheckField { id: advanced; text: qsTr("Advanced run settings"); Layout.topMargin: 8 }
            GridLayout {
                visible: advanced.checked; columns: 2; Layout.fillWidth: true; columnSpacing: 16; rowSpacing: 8
                Label { text: qsTr("Request timeout · seconds"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("Concurrent analysis tasks"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: timeout; validator: IntValidator { bottom: 1 } Layout.fillWidth: true }
                Field { id: agents; validator: IntValidator { bottom: 1 } Layout.fillWidth: true }
                Label { text: qsTr("Maximum subtask depth"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("Reasoning effort"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: depth; validator: IntValidator { bottom: 0 } Layout.fillWidth: true }
                ChoiceField { id: effort; model: [qsTr("Server default"), qsTr("Low"), qsTr("Medium"), qsTr("High"), qsTr("Maximum")]; Layout.fillWidth: true }
                Label { text: qsTr("Anthropic API version"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("Chat output field"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: version; Layout.fillWidth: true }
                ChoiceField { id: tokenField; model: ["max_completion_tokens", "max_tokens"]; Layout.fillWidth: true }
            }
            Label {
                visible: advanced.checked
                text: qsTr("Model call counts are for progress only; they do not limit calls or force a task to finish. Independent objects are analyzed concurrently, and objects beyond the concurrent task limit wait in a queue. A persistent goal's total token budget is still shared by the whole task.")
                color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true
            }
            Label { text: backend.connectionStatus; visible: text.length > 0; color: Theme.text; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            Label { text: backend.credentialStatus; visible: text.length > 0; color: Theme.secondary; wrapMode: Text.WrapAnywhere; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            ActionButton { text: qsTr("Reload key from system keychain"); visible: dialog.hasSavedKey; enabled: !backend.credentialBusy; onClicked: backend.reloadCredential() }
            Label { text: qsTr("After saving, new tasks use the new settings; running tasks keep their original settings."); color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
        }
    }
    footer: RowLayout {
        spacing: 8
        ActionButton {
            Layout.leftMargin: 18; Layout.bottomMargin: 16
            text: backend.testingConnection ? qsTr("Stop test") : qsTr("Test connection")
            onClicked: backend.testingConnection ? backend.cancelConnectionTest() : backend.testConnection(dialog.values(), secret.text, removeKey.checked)
        }
        Item { Layout.fillWidth: true }
        ActionButton { text: qsTr("Cancel"); Layout.bottomMargin: 16; onClicked: dialog.close() }
        ActionButton {
            text: qsTr("Save"); primary: true; Layout.rightMargin: 18; Layout.bottomMargin: 16
            enabled: !backend.credentialBusy
            onClicked: backend.updateModelSettings(dialog.values(), secret.text, removeKey.checked, mcp.text)
        }
    }
}
