pragma Singleton
import QtQuick

// The workbench's look: color roles from ChromeColors.def (Visual Studio Code
// Dark+ and Light+), its corners and type, so Agent reads as part of NeverD.
// A color here names a role of that table, never a literal of its own.
QtObject {
    id: theme
    property bool light: false
    property Item backdrop: null
    property real basePointSize: 10.5
    property font baseFont: Qt.application.font
    function chrome(name) { return windowFrame.chromeColor(name, light) }
    // Controls grow with the desktop's interface font: a line of text and
    // the workbench's 4-pixel padding above and below it, at least 28.
    readonly property FontMetrics baseMetrics: FontMetrics { font: theme.baseFont }
    readonly property int controlHeight: Math.max(28, Math.ceil(baseMetrics.height) + 8)
    // Corners as in the workbench: fields and push buttons 2, tool buttons,
    // check boxes and menu highlights 3, cards and popups 6, panes none.
    readonly property int controlRadius: 2
    readonly property int toolRadius: 3
    readonly property int iconSize: 17
    readonly property int spaceSmall: 8
    readonly property int spaceMedium: 16
    readonly property int spaceLarge: 24
    // Panes are surfaces a step apart, parted by hairlines: the side panes
    // take the window's fill, the conversation the fill of views and lists.
    readonly property color background: chrome("Window")
    readonly property color surface: chrome("Window")
    readonly property color content: chrome("Base")
    readonly property color raised: chrome("Input")
    readonly property color userBubble: chrome("Window")
    readonly property color assistantBubble: "transparent"
    readonly property color bubbleBorder: chrome("Border")
    readonly property color code: chrome("Window")
    readonly property color hover: chrome("Hover")
    readonly property color pressed: chrome("ToolBarHover")
    readonly property color selected: chrome("ListSelection")
    readonly property color selectedText: chrome("ListSelectionText")
    readonly property color selection: chrome("Highlight")
    readonly property color selectionText: chrome("HighlightedText")
    readonly property color border: chrome("Separator")
    readonly property color borderStrong: chrome("FloatingWindowBorder")
    readonly property color inputBorder: chrome("InputBorder")
    readonly property color shadow: chrome("Shadow")
    readonly property color text: chrome("Text")
    readonly property color secondary: chrome("SecondaryText")
    readonly property color quiet: chrome("PlaceholderText")
    readonly property color disabledText: chrome("DisabledText")
    readonly property color composerTint: chrome("Input")
    readonly property color focusBorder: chrome("Focus")
    readonly property color accent: chrome("Accent")
    readonly property color accentLine: chrome("Accent")
    readonly property color link: chrome("Link")
    readonly property color focus: chrome("Focus")
    // The one main button of a window, and every other push button.
    readonly property color primary: chrome("ButtonPrimary")
    readonly property color primaryHover: chrome("ButtonPrimaryHover")
    readonly property color primaryText: chrome("ButtonPrimaryText")
    readonly property color primaryDisabled: chrome("ButtonPrimaryDisabled")
    readonly property color primaryDisabledText: chrome("ButtonPrimaryDisabledText")
    readonly property color button: chrome("Button")
    readonly property color buttonHover: chrome("ButtonHover")
    readonly property color buttonText: chrome("ButtonText")
    readonly property color controlDisabled: chrome("ControlDisabled")
    readonly property color toolHover: chrome("ToolBarHover")
    readonly property color toolChecked: chrome("ToolButtonChecked")
    readonly property color toolCheckedText: chrome("ToolButtonCheckedText")
    readonly property color checkBox: chrome("CheckboxBackground")
    readonly property color checkBoxBorder: chrome("CheckboxBorder")
    readonly property color checkBoxChecked: chrome("CheckboxChecked")
    readonly property color menu: chrome("Menu")
    readonly property color menuText: chrome("MenuText")
    readonly property color menuSelection: chrome("MenuSelection")
    readonly property color menuSelectionText: chrome("MenuSelectionText")
    readonly property color menuSeparator: chrome("MenuSeparator")
    readonly property color scrollbar: chrome("ScrollbarSlider")
    readonly property color scrollbarHover: chrome("ScrollbarSliderHover")
    readonly property color scrollbarActive: chrome("ScrollbarSliderActive")
    readonly property color statusBar: chrome("StatusBar")
    readonly property color statusBarText: chrome("StatusBarText")
    readonly property color statusBarBorder: chrome("StatusBarBorder")
    readonly property color titleBar: chrome("TitleBar")
    readonly property color titleText: chrome("TitleBarText")
    readonly property color titleInactiveText: chrome("TitleBarInactiveText")
    readonly property color controlHover: chrome("WindowControlHover")
    readonly property color controlPressed: chrome("WindowControlPressed")
    readonly property color closeHover: chrome("WindowCloseHover")
    readonly property color closePressed: chrome("WindowClosePressed")
    readonly property color closeText: chrome("WindowCloseText")
    readonly property color windowEdge: chrome("WindowEdge")
    readonly property color windowShadow: chrome("WindowShadow")
    readonly property color error: windowFrame.themeColor("ListingError", light)
    // One reading size, the desktop's: secondary text is quieted by color.
    readonly property real smallPointSize: basePointSize
    readonly property real bodyPointSize: basePointSize
    readonly property real titlePointSize: basePointSize
    readonly property real radius: 6
    readonly property real workspaceRadius: 0
}
