import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import NeverD.Agent

// A row of the side list. The whole row answers the pointer and carries the
// quiet fill of the one that is open; its menu button shows while the pointer
// or the keyboard is on it. A row that folds others away shows its glyph until
// then, and the chevron that folds them in its place.
Item {
    id: row
    property string text: ""
    property string glyph: "file"
    property string hint: text
    property string foldHint: ""
    property string menuHint: ""
    property bool selected: false
    property bool foldable: false
    property bool expanded: true
    // A dot for something that waits for the person.
    property bool marked: false
    // Quieter text, for something listed that cannot be opened now.
    property bool dimmed: false
    property int indent: 0
    property int elideMode: Text.ElideRight
    property int weight: Font.Normal
    signal clicked()
    signal foldRequested()
    signal menuRequested(var origin)
    implicitHeight: Theme.rowHeight
    // The pointer on the row, or the keyboard: a click leaves the row as it
    // was, so its glyph comes back when the pointer goes.
    readonly property bool engaged: pointer.hovered || nameButton.visualFocus
                                    || foldButton.visualFocus || menuButton.visualFocus
    // A click or a menu answers the hint: it stays away until the pointer
    // comes back, so it never lies over the menu the row opened.
    property bool hintAnswered: false
    HoverHandler { id: pointer; onHoveredChanged: if (!hovered) row.hintAnswered = false }
    Rectangle {
        anchors.fill: parent
        radius: Theme.toolRadius
        color: row.selected ? Theme.rowSelected : nameButton.down ? Theme.toolHover
             : pointer.hovered ? Theme.hover : "transparent"
        border.width: nameButton.visualFocus || (Theme.highContrast && (row.selected || pointer.hovered)) ? 1 : 0
        border.color: nameButton.visualFocus ? Theme.focusBorder : Theme.toolHoverBorder
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 4 + row.indent
        anchors.rightMargin: 2
        spacing: 0
        ActionButton {
            id: foldButton
            visible: row.foldable
            glyph: row.engaged ? (row.expanded ? "chevron_down" : "chevron_right") : row.glyph
            hint: row.foldHint
            focusPolicy: Qt.TabFocus
            implicitWidth: 24
            implicitHeight: 24
            leftPadding: 4
            rightPadding: 4
            onClicked: row.foldRequested()
        }
        Item {
            visible: !row.foldable
            Layout.preferredWidth: 24
            Layout.preferredHeight: 24
            Icon {
                anchors.centerIn: parent
                width: Theme.iconSize
                height: Theme.iconSize
                name: row.glyph
                color: row.selected ? Theme.rowSelectedText : row.dimmed ? Theme.quiet
                     : pointer.hovered ? Theme.text : Theme.secondary
            }
        }
        AbstractButton {
            id: nameButton
            Layout.fillWidth: true
            Layout.fillHeight: true
            focusPolicy: Qt.StrongFocus
            leftPadding: 4
            rightPadding: 4
            Accessible.name: row.text
            contentItem: Text {
                text: row.text
                textFormat: Text.PlainText
                color: row.selected ? Theme.rowSelectedText : row.dimmed ? Theme.quiet : Theme.text
                font.family: nameButton.font.family
                font.pointSize: nameButton.font.pointSize
                font.weight: row.weight
                elide: row.elideMode
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: { row.hintAnswered = true; row.clicked() }
            Keys.onMenuPressed: row.menuRequested(nameButton)
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: { row.hintAnswered = true; row.menuRequested(nameButton) }
            }
            ThemedToolTip {
                visible: nameButton.hovered && !row.hintAnswered && row.hint.length > 0 && row.hint !== row.text
                text: row.hint
            }
        }
        Rectangle {
            visible: row.marked
            Layout.preferredWidth: 6
            Layout.preferredHeight: 6
            Layout.rightMargin: 6
            radius: 3
            color: Theme.accent
        }
        ActionButton {
            id: menuButton
            visible: row.menuHint.length > 0
            glyph: "more"
            hint: row.menuHint
            focusPolicy: Qt.TabFocus
            opacity: row.engaged ? 1 : 0
            implicitWidth: 26
            implicitHeight: 26
            leftPadding: 5
            rightPadding: 5
            onClicked: row.menuRequested(menuButton)
        }
    }
}
