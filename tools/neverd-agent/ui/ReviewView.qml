import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

ColumnLayout {
    id: view
    required property var backend
    spacing: 10
    Label {
        Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondary
        text: view.backend.reviewBusy ? qsTr("Verifying and saving NeverD changes…") : qsTr("Review each change and its evidence before applying it to the NeverD database. The original binary is not modified.")
    }
    ScrollView {
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
        ColumnLayout {
            width: parent.width; spacing: 14
            Label { visible: view.backend.changes.length === 0; text: qsTr("No change proposals yet.\nDuring analysis, you can ask for renames, comments or refined types."); color: Theme.quiet; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Repeater {
                model: view.backend.changes
                delegate: Surface {
                    id: card
                    required property var modelData
                    Layout.fillWidth: true; implicitHeight: content.implicitHeight + 28
                    radius: Theme.radius
                    // A card a step apart from the pane, as the workbench
                    // sets its floating cards: the view fill and a hairline.
                    color: Theme.content
                    border.width: 1
                    border.color: Theme.border
                    ColumnLayout {
                        id: content
                        anchors.fill: parent; anchors.margins: 14; spacing: 8
                        Label { text: card.modelData.objectName + " · " + (card.modelData.address || qsTr("Local types")); color: Theme.text; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                        Label {
                            text: ({rename:qsTr("Rename"),comment:qsTr("Comment"),function_type:qsTr("Function type"),types:qsTr("Type declarations")}[card.modelData.kind] || card.modelData.kind) + " · " +
                                  ({pending:qsTr("Pending review"),applied:qsTr("Applied"),undone:qsTr("Undone"),dismissed:qsTr("Dismissed"),conflict:qsTr("Current value changed"),failed:qsTr("Failed"),uncertain:qsTr("Write result unconfirmed"),applied_unverified:qsTr("Saved; receipt not confirmed"),no_change:qsTr("No new changes")}[card.modelData.status] || card.modelData.status)
                            color: Theme.secondary; wrapMode: Text.Wrap; Layout.fillWidth: true
                        }
                        Label { text: card.modelData.rationale; color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        Label { text: qsTr("Before"); color: Theme.quiet }
                        MarkdownView { source: card.modelData.beforeDisplay || qsTr("(empty)"); markdown: false; Layout.fillWidth: true; font.family: "JetBrains Mono NL" }
                        Label { text: qsTr("After"); color: Theme.quiet }
                        MarkdownView { source: card.modelData.after || qsTr("(cleared)"); markdown: false; Layout.fillWidth: true; font.family: "JetBrains Mono NL" }
                        Repeater {
                            model: card.modelData.evidenceIds
                            ActionButton { required property string modelData; quiet: true; glyph: "link"; text: qsTr("View evidence · %1").arg(modelData.slice(0, 8)); onClicked: view.backend.openEvidence(modelData) }
                        }
                        Label { visible: !!card.modelData.error; text: card.modelData.error || ""; color: Theme.error; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                        Flow {
                            Layout.fillWidth: true; spacing: 6
                            ActionButton { visible: card.modelData.status === "pending" || card.modelData.status === "failed"; text: qsTr("Apply change"); enabled: !view.backend.reviewBusy && !view.backend.running; onClicked: view.backend.applyChange(card.modelData.id) }
                            ActionButton { visible: ["pending","failed","conflict"].includes(card.modelData.status); quiet: true; text: qsTr("Dismiss"); enabled: !view.backend.reviewBusy; onClicked: view.backend.dismissChange(card.modelData.id) }
                            ActionButton { visible: card.modelData.status === "applied"; text: qsTr("Undo this change"); enabled: !view.backend.reviewBusy && !view.backend.running; onClicked: view.backend.applyChange(card.modelData.id, true) }
                        }
                    }
                }
            }
        }
    }
    ActionButton { text: qsTr("Refresh"); glyph: "refresh"; onClicked: view.backend.refreshResearch() }
}
