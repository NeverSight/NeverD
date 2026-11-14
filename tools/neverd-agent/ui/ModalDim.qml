import QtQuick
import NeverD.Agent

// A modal popup's dimming, over the window itself and not the transparent
// band where the frame draws its shadow.
Item {
    Rectangle {
        anchors.fill: parent
        anchors.margins: windowFrame.shadowMargin
        color: Theme.dim
    }
}
