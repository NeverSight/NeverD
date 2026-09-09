pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property var controller
    property var kddockwidgets_min_size: Qt.size(280, 100)
    color: Theme.editor
    ListView {
        id: references
        objectName: "referencesList"
        anchors.fill: parent
        clip: true
        reuseItems: true
        model: root.controller.xrefsModel
        LayoutMirroring.enabled: false
        ScrollBar.vertical: ScrollBar { active: true }
        delegate: ItemDelegate {
            id: reference
            required property string from
            required property string to
            required property string kind
            width: references.width
            height: 29
            leftPadding: 18
            rightPadding: 18
            hoverEnabled: true
            contentItem: RowLayout {
                spacing: 24
                Text { textFormat: Text.PlainText; text: reference.kind; color: Theme.muted; font.pointSize: Theme.captionSize; Layout.preferredWidth: 145; elide: Text.ElideRight }
                Text { textFormat: Text.PlainText; text: reference.from; color: Theme.variable; font.family: Theme.monoFont; font.pointSize: Theme.bodySize; Layout.preferredWidth: 170 }
                Text { textFormat: Text.PlainText; text: "→"; color: Theme.subdued }
                Text { textFormat: Text.PlainText; text: reference.to; color: Theme.functionName; font.family: Theme.monoFont; font.pointSize: Theme.bodySize; Layout.fillWidth: true; elide: Text.ElideRight }
            }
            background: Rectangle { color: reference.hovered ? Theme.hover : "transparent"; border.color: reference.activeFocus ? Theme.focus : "transparent" }
            onClicked: root.controller.navigate(from)
            Accessible.name: from + " " + kind + " " + to
        }
    }
    Text { textFormat: Text.PlainText; anchors.centerIn: parent; visible: references.count === 0; text: root.controller.loaded ? qsTranslate("Main", "No references for this selection.") : qsTranslate("Main", "References to the selected address appear here."); color: Theme.subdued; font.pointSize: Theme.bodySize; width: parent.width - 36; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter }
}
