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
    width: Math.min(640, parent.width - 48)
    height: Math.min(implicitHeight, parent.height - 48)
    padding: 18
    topPadding: 4
    background: Surface { glass: true; radius: Theme.radius; color: Theme.background }
    header: Label { text: qsTr("模型与分析设置"); color: Theme.text; font.pointSize: Theme.bodyPointSize; font.weight: Font.Medium; padding: 18 }
    function values() {
        return {protocol: protocols[protocol.currentIndex], url: endpoint.text.trim(), model: modelName.text.trim(),
            contextTokens: Number(contextSize.text), outputTokens: Number(outputSize.text),
            timeoutSeconds: Number(timeout.text), maxAgents: Number(agents.text),
            maxDepth: Number(depth.text), anthropicVersion: version.text,
            completionTokenField: tokenField.currentIndex === 0 ? "max_completion_tokens" : "max_tokens",
            reasoningEffort: efforts[effort.currentIndex]}
    }
    function load() {
        baseline = backend.modelSettings
        protocol.currentIndex = Math.max(0, protocols.indexOf(baseline.protocol))
        endpoint.text = baseline.url; modelName.text = baseline.model
        contextSize.text = String(baseline.contextTokens); outputSize.text = String(baseline.outputTokens)
        timeout.text = String(baseline.timeoutSeconds)
        agents.text = String(baseline.maxAgents); depth.text = String(baseline.maxDepth)
        version.text = baseline.anthropicVersion
        tokenField.currentIndex = baseline.completionTokenField === "max_tokens" ? 1 : 0
        effort.currentIndex = Math.max(0, efforts.indexOf(baseline.reasoningEffort || ""))
        mcp.text = backend.mcpPath; secret.clear(); removeKey.checked = false; reveal.checked = false
    }
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
            Label { text: qsTr("API 协议"); color: Theme.secondary; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            ChoiceField { id: protocol; model: ["OpenAI Responses", "Anthropic Messages", "OpenAI Chat Completions"]; Layout.fillWidth: true }
            Label { text: qsTr("API URL"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
            Field { id: endpoint; Layout.fillWidth: true; placeholderText: "https://api.openai.com/v1" }
            Label { text: qsTr("支持 API 根地址或完整接口地址。自建服务可使用 HTTP。"); color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            Label { text: qsTr("API Key"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
            Field {
                id: secret
                Layout.fillWidth: true
                echoMode: reveal.checked ? TextInput.Normal : TextInput.Password
                placeholderText: dialog.baseline.hasKey ? qsTr("已保存 · 留空保留当前 Key") : qsTr("填写 API Key；本地无鉴权服务可留空")
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            }
            RowLayout {
                CheckField { id: reveal; text: qsTr("显示新输入的 Key") }
                CheckField { id: removeKey; text: qsTr("清除已保存的 Key") }
            }
            Label { text: qsTr("模型名称"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
            Field { id: modelName; Layout.fillWidth: true; placeholderText: qsTr("填写服务端模型 ID") }
            GridLayout {
                Layout.fillWidth: true
                columns: 2; columnSpacing: 16; rowSpacing: 8
                Label { text: qsTr("上下文窗口 · tokens"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("单次输出上限 · tokens"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: contextSize; Layout.fillWidth: true; validator: IntValidator { bottom: 1 } }
                Field { id: outputSize; Layout.fillWidth: true; validator: IntValidator { bottom: 1 } }
            }
            Label {
                text: qsTr("预留输出空间，在可用输入额度的 80% 自动压缩。保留当前问题、关键证据和最近完整调用，原始记录仍在项目中。服务端用量用于校准本地估算。")
                color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true
            }
            Label { text: qsTr("ND MCP 程序路径"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
            Field { id: mcp; Layout.fillWidth: true; placeholderText: qsTr("neverd-mcp；留空查找相邻目录、ND GUI 目录或 PATH") }
            Label { text: qsTr("分析使用 ND 工具。每个对象独立进程，文件只读。"); color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            CheckField { id: advanced; text: qsTr("高级运行设置") }
            GridLayout {
                visible: advanced.checked; columns: 2; Layout.fillWidth: true; columnSpacing: 16; rowSpacing: 8
                Label { text: qsTr("请求超时 · 秒"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("同时存活的分析任务数"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: timeout; validator: IntValidator { bottom: 1 } Layout.fillWidth: true }
                Field { id: agents; validator: IntValidator { bottom: 1 } Layout.fillWidth: true }
                Label { text: qsTr("子任务最大深度"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("思考强度"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: depth; validator: IntValidator { bottom: 0 } Layout.fillWidth: true }
                ChoiceField { id: effort; model: [qsTr("跟随服务端"), "low", "medium", "high", "max"]; Layout.fillWidth: true }
                Label { text: qsTr("Anthropic API 版本"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Label { text: qsTr("Chat 输出字段"); color: Theme.secondary; font.pointSize: Theme.smallPointSize }
                Field { id: version; Layout.fillWidth: true }
                ChoiceField { id: tokenField; model: ["max_completion_tokens", "max_tokens"]; Layout.fillWidth: true }
            }
            Label {
                visible: advanced.checked
                text: qsTr("模型调用次数仅用于进度统计，不限制调用次数或强制收尾。独立对象并发分析，超过同时任务数的对象排队等待。持续目标的 token 总预算仍由整个任务共享。")
                color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true
            }
            Label { text: backend.connectionStatus; visible: text.length > 0; color: Theme.text; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            Label { text: backend.credentialStatus; visible: text.length > 0; color: Theme.secondary; wrapMode: Text.WrapAnywhere; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
            ActionButton { text: qsTr("重新读取系统凭据"); visible: backend.modelSettings.hasKey; enabled: !backend.credentialBusy; onClicked: backend.reloadCredential() }
            Label { text: qsTr("保存后新任务使用新配置，正在运行的任务保持原配置。"); color: Theme.secondary; wrapMode: Text.Wrap; font.pointSize: Theme.smallPointSize; Layout.fillWidth: true }
        }
    }
    footer: RowLayout {
        spacing: 8
        ActionButton {
            Layout.leftMargin: 18; Layout.bottomMargin: 16
            text: backend.testingConnection ? qsTr("停止测试") : qsTr("测试连接")
            onClicked: backend.testingConnection ? backend.cancelConnectionTest() : backend.testConnection(dialog.values(), secret.text, removeKey.checked)
        }
        Item { Layout.fillWidth: true }
        ActionButton { text: qsTr("取消"); Layout.bottomMargin: 16; onClicked: dialog.close() }
        ActionButton {
            text: qsTr("保存"); primary: true; Layout.rightMargin: 18; Layout.bottomMargin: 16
            enabled: !backend.credentialBusy
            onClicked: backend.updateModelSettings(dialog.values(), secret.text, removeKey.checked, mcp.text)
        }
    }
}
