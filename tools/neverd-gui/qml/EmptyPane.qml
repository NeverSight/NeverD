pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

Item {
    property string title: ""
    property string detail: ""
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 420)
        spacing: 10
        Text {
            Layout.fillWidth: true
            text: title
            font.pointSize: Theme.titleSize
            font.weight: Font.Medium
            color: Theme.foreground
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            text: detail
            font.pointSize: Theme.bodySize
            color: Theme.muted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: 1.4
        }
    }
}
