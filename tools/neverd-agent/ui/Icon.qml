import QtQuick
import QtQuick.Shapes
import NeverD.Agent

Item {
    id: icon
    property string name: ""
    property color color: Theme.secondary
    implicitWidth: 20
    implicitHeight: 20
    readonly property var paths: ({
        folder: "M3 7V5.5Q3 4 4.5 4H9L11 6H19.5Q21 6 21 7.5V18.5Q21 20 19.5 20H4.5Q3 20 3 18.5Z",
        folder_new: "M3 7V5.5Q3 4 4.5 4H9L11 6H19.5Q21 6 21 7.5V18.5Q21 20 19.5 20H4.5Q3 20 3 18.5Z M12 10V16 M9 13H15",
        file: "M14 3H6Q4 3 4 5V19Q4 21 6 21H18Q20 21 20 19V9Z M14 3V9H20",
        open: "M3 10V5.5Q3 4 4.5 4H9L11 6H19Q21 6 21 8V10 M3 10H22L19 20H3Z",
        options: "M4 6H20 M4 12H20 M4 18H20 M8 3V9 M16 9V15 M10 15V21",
        subviews: "M5 4H19Q21 4 21 6V18Q21 20 19 20H5Q3 20 3 18V6Q3 4 5 4Z M15 4V20",
        graph: "M4 3H10V9H4Z M14 15H20V21H14Z M4 15H10V21H4Z M7 9V12H17V15 M7 12V15",
        comment: "M5 4H19Q21 4 21 6V16Q21 18 19 18H9L4 21V18Q2 18 2 16V7Q2 4 5 4Z M7 9H16 M7 13H13",
        bookmark: "M6 21V5Q6 3 8 3H16Q18 3 18 5V21L12 17Z",
        float: "M14 3H21V10 M21 3L11 13 M10 4H5Q3 4 3 6V19Q3 21 5 21H18Q20 21 20 19V14",
        close_small: "M6 6L18 18 M18 6L6 18",
        forward: "M5 12H19 M13 6L19 12L13 18",
        refresh: "M20 10A8 8 0 1 0 19 17 M20 4V10H14",
        file_load: "M5 15V19Q5 21 7 21H17Q19 21 19 19V15 M12 3V15 M7 10L12 15L17 10",
        file_produce: "M5 15V19Q5 21 7 21H17Q19 21 19 19V15 M12 15V3 M7 8L12 3L17 8",
        plus: "M12 5V19 M5 12H19",
        check: "M5 12.5L10 17L19 7",
        chevron_down: "M6 9L12 15L18 9",
        chevron_right: "M9 6L15 12L9 18",
        more: "M5 12H5.1 M12 12H12.1 M19 12H19.1",
        send: "M12 19V5 M6 11L12 5L18 11",
        stop: "M7 6H17Q18 6 18 7V17Q18 18 17 18H7Q6 18 6 17V7Q6 6 7 6Z",
        copy: "M9 8H18Q20 8 20 10V19Q20 21 18 21H9Q7 21 7 19V10Q7 8 9 8Z M16 5V4Q16 2 14 2H5Q3 2 3 4V13Q3 15 5 15",
        clock: "M21 12A9 9 0 1 1 3 12A9 9 0 1 1 21 12 M12 7V12L15 14"
    })
    Shape {
        width: 24; height: 24
        scale: Math.min(icon.width, icon.height) / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: icon.color
            strokeWidth: icon.name === "more" ? 2.6 : 1.65
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: icon.paths[icon.name] || "" }
        }
    }
}
