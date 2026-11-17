pragma Singleton
import QtQuick

// The look of NeverD Agent. Its colors are the workbench's: every color here
// names a role of ChromeColors.def (Visual Studio Code Dark+ and Light+ and
// the high contrast themes), never a literal of its own, and the theme is the
// workbench's own choice. Its shapes are a conversation's, not a tool
// window's: soft corners, roomy rows, a type scale and one reading column, on
// a grid of 4 pixels.
QtObject {
    id: theme
    readonly property int mode: appearance.themeMode
    readonly property bool light: appearance.light
    readonly property bool highContrast: appearance.highContrast
    property Item backdrop: null
    property real basePointSize: 10.5
    property font baseFont: Qt.application.font
    function chrome(name) { return appearance.chromeColor(name, mode) }
    // Sizes grow with the desktop's interface font. A row of a list or a
    // tool button is a line of text and 5 pixels above and below it, at least
    // 30; a push button or a field is 2 taller.
    readonly property FontMetrics baseMetrics: FontMetrics { font: theme.baseFont }
    readonly property int rowHeight: Math.max(30, Math.ceil(baseMetrics.height) + 10)
    readonly property int controlHeight: rowHeight + 2
    // Corners: rows, buttons, fields and menu items 8, a check box half of
    // that, cards, menus and dialogs 12, the composer and a message 20. The
    // window and its panes have none.
    readonly property int controlRadius: 8
    readonly property int toolRadius: 8
    readonly property int checkRadius: 4
    readonly property real radius: 12
    readonly property int largeRadius: 20
    readonly property int iconSize: 16
    readonly property int spaceTiny: 4
    readonly property int spaceSmall: 8
    readonly property int spaceMedium: 16
    readonly property int spaceLarge: 24
    // The strip that carries the window's title and controls, the side list,
    // and the column a conversation is read in.
    readonly property int topBarHeight: 44
    readonly property int sidebarWidth: 272
    readonly property int sidebarNarrowWidth: 232
    readonly property int readingWidth: 768
    readonly property int dialogPadding: 24
    // The side list and the panel take the window's fill and the
    // conversation the fill of views and lists, a step apart. Only the high
    // contrast themes, where the two fills are one, part them with a line.
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
    // The row or toggle that is on: a quiet fill, so the window's one accent
    // stays on its main button. High contrast fills nothing there and uses
    // the checked tool button's colors.
    readonly property color rowSelected: highContrast ? chrome("ToolButtonChecked") : chrome("ToolBarHover")
    readonly property color rowSelectedText: highContrast ? chrome("ToolButtonCheckedText") : chrome("Text")
    readonly property color paneLine: highContrast ? chrome("Separator") : "transparent"
    readonly property color selected: chrome("ListSelection")
    readonly property color selectedText: chrome("ListSelectionText")
    readonly property color selection: chrome("Highlight")
    readonly property color selectionText: chrome("HighlightedText")
    readonly property color border: chrome("Separator")
    readonly property color borderStrong: chrome("FloatingWindowBorder")
    readonly property color inputBorder: chrome("InputBorder")
    // High contrast outlines what the other themes fill.
    readonly property color primaryBorder: chrome("ButtonPrimaryBorder")
    readonly property color toolHoverBorder: chrome("ToolBarHoverBorder")
    readonly property color shadow: chrome("Shadow")
    readonly property color text: chrome("Text")
    readonly property color secondary: chrome("SecondaryText")
    readonly property color quiet: chrome("PlaceholderText")
    readonly property color disabledText: chrome("DisabledText")
    readonly property color focusBorder: chrome("Focus")
    readonly property color accent: chrome("Accent")
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
    readonly property color titleText: chrome("TitleBarText")
    readonly property color titleInactiveText: chrome("TitleBarInactiveText")
    readonly property color controlHover: chrome("WindowControlHover")
    readonly property color controlPressed: chrome("WindowControlPressed")
    readonly property color closeHover: chrome("WindowCloseHover")
    readonly property color closePressed: chrome("WindowClosePressed")
    readonly property color closeText: chrome("WindowCloseText")
    readonly property color windowEdge: chrome("WindowEdge")
    readonly property color windowShadow: chrome("WindowShadow")
    readonly property color error: appearance.themeColor("ListingError", mode)
    // The type scale, in steps of the desktop's interface size: notes a
    // little under it, a sheet's title a step above it, and one line twice
    // its size where a window has nothing else to say.
    readonly property real smallPointSize: basePointSize * 0.93
    readonly property real bodyPointSize: basePointSize
    readonly property real titlePointSize: basePointSize * 1.15
    readonly property real heroPointSize: basePointSize * 2
    // Behind a modal dialog, as Qt's Basic style dims, a little lighter in
    // the light themes.
    readonly property color dim: Qt.rgba(0, 0, 0, light ? 0.3 : 0.5)
    readonly property real workspaceRadius: 0
}
