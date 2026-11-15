import QtQuick

// The bare part of the strip along the window's top: dragging it moves the
// window, a double click maximizes or restores it.
MouseArea {
    property point pressAt
    property bool moving: false
    onPressed: mouse => { pressAt = Qt.point(mouse.x, mouse.y); moving = false }
    onPositionChanged: mouse => {
        if (pressed && !moving && Math.abs(mouse.x - pressAt.x) + Math.abs(mouse.y - pressAt.y) >= windowFrame.dragDistance) {
            moving = true
            windowFrame.startMove()
        }
    }
    onDoubleClicked: windowFrame.toggleMaximized()
}
