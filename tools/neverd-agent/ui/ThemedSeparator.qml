import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

MenuSeparator {
    implicitHeight: visible ? 9 : 0
    topPadding: 4
    bottomPadding: 4
    leftPadding: 6
    rightPadding: 6
    contentItem: Rectangle { implicitHeight: 1; color: Theme.menuSeparator }
}
