import QtQuick
import QtQuick.Controls.Basic
import NeverD.Agent

// A menu: the menu surface inside a hairline, a card's corners, inset 4.
Menu {
    id: menu
    padding: 4
    margins: 8
    width: 200
    // As wide as its longest command, in any language, within reason.
    function fit() {
        let widest = 0
        for (let i = 0; i < count; ++i) {
            const item = itemAt(i)
            if (item && item.visible)
                widest = Math.max(widest, item.implicitWidth)
        }
        width = Math.min(Math.max(180, Math.ceil(widest) + leftPadding + rightPadding), 440)
    }
    onAboutToShow: fit()
    delegate: ThemedMenuItem {}
    background: Rectangle {
        implicitWidth: menu.width
        radius: Theme.radius
        color: Theme.menu
        border.width: 1
        border.color: Theme.borderStrong
    }
}
