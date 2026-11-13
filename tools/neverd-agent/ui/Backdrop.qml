import QtQuick
import NeverD.Agent

// Flat window ground with rounded corners.
Rectangle {
    property real cornerRadius: 8
    radius: cornerRadius
    color: Theme.background
    antialiasing: true
}
