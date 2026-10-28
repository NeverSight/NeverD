//===- Config.h - Desktop presentation defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Layout, font and zoom defaults for the NeverD workbench.
/// Keep desktop presentation policy separate from request admission ceilings.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_CONFIG_H
#define NEVERD_TOOLS_NEVERD_GUI_CONFIG_H

#include <QSize>
#include <QtGlobal>
#include <cstddef>
#include <cstdint>

namespace neverd::limits::gui_config {

// tools/neverd-gui/app/ExtensionsView.cpp
/// The narrowest each pane may get.  A scroll area asks at least for its
/// scroll bars' room, and macOS's take enough that the two panes side by
/// side held the dock wider than the views around it.
inline constexpr int kExtensionsViewMinimumPaneWidth = 48;

// tools/neverd-gui/app/GraphView.cpp
inline constexpr qreal kGraphViewMaxOverviewScale = 0.5;
inline constexpr qreal kGraphViewMinimumZoom = 0.02;
inline constexpr qreal kGraphViewMaximumZoom = 4.0;
inline constexpr qreal kGraphViewTextPixelThreshold = 5.0;

// tools/neverd-gui/app/LocalTypesView.cpp
/// The narrowest each pane may get, as the extensions window keeps.
inline constexpr int kLocalTypesViewMinimumPaneWidth = 48;

// tools/neverd-gui/app/MainWindow.cpp
inline constexpr int kMainWindowMinimumFunctionsWidth = 240;
/// The digits of the input's SHA-256 a copied link names it by: 64 bits tell
/// builds apart, and the link stays short.
inline constexpr int kCopiedLinkHashDigits = 16;
inline constexpr int kMainWindowMaximumFunctionsWidth = 420;
inline constexpr int kMainWindowMinimumOutputHeight = 120;
inline constexpr int kMainWindowMaximumOutputHeight = 260;
// Three deliberate taps enter focus mode; typing and held keys do not.
inline constexpr int kFocusModeSequenceTimeoutMs = 1000;
inline constexpr int kFocusModeSequenceLength = 3;

// tools/neverd-gui/app/FocusSidebar.cpp
inline constexpr int kFocusSidebarPreferredWidth = 360;
inline constexpr int kFocusSidebarMinimumWidth = 240;
inline constexpr int kFocusSidebarKeyboardResizeStep = 16;
inline constexpr int kFocusSidebarToggleWidth = 20;
inline constexpr int kFocusSidebarToggleHeight = 36;
inline constexpr int kFocusSidebarBorderWidth = 1;

// tools/neverd-gui/app/QuickStartDialog.cpp
/// The widest the side grows for a translation's longer actions.
inline constexpr int kQuickStartDialogMaximumSideWidth = 440;

// tools/neverd-gui/app/StyledText.cpp
/// Pixels a character's advance may differ from its columns' width.
inline constexpr qreal kStyledTextGridTolerance = 0.01;

// tools/neverd-gui/app/Theme.cpp
inline constexpr int kThemeMinimumCodePointSize = 6;
inline constexpr int kThemeMaximumCodePointSize = 48;

// tools/neverd-gui/app/TitleBar.cpp
inline constexpr int kTitleBarCenterMaxWidth = 600;
/// Narrower than this, the command center gives way to the menus.
inline constexpr int kTitleBarCenterMinWidth = 120;

// tools/neverd-gui/main.cpp
inline constexpr int kMainDefaultWidth = 1600;
inline constexpr int kMainDefaultHeight = 1000;
/// The share of the screen a window opens at without a remembered geometry:
/// below the 80% of its area at which GNOME maximizes a new window, whose
/// edges then no longer resize it.
inline constexpr int kMainDefaultWidthPercent = 75;
inline constexpr int kMainDefaultHeightPercent = 80;

// tools/neverd-gui/app/SetTypeDialog.cpp
inline constexpr int kSetTypeDialogCompletedWidthMaxArgument1 = 320;

// tools/neverd-gui/app/CodeView.cpp
inline constexpr int kCodeViewMargin = 6;

// tools/neverd-gui/app/Docking.cpp
// Docked windows are one hairline apart.  A separator's grab area spreads
// over both neighbors, like the sashes of Visual Studio Code; an odd width
// centers it on the hairline.
inline constexpr int kDockingSeparatorLineWidth = 1;
inline constexpr int kDockingSeparatorGrabWidth = 5;
// The bar a hovered or dragged separator shows, centered on the hairline.
inline constexpr qreal kDockingSeparatorHighlightWidth = 3;

// tools/neverd-gui/app/FunctionEditDialog.cpp
/// The dialog's margins and the space between its columns and rows, on the
/// 4-pixel grid.
inline constexpr int kFunctionEditDialogMargin = 16;
inline constexpr int kFunctionEditDialogColumnSpacing = 16;
inline constexpr int kFunctionEditDialogRowSpacing = 8;

// tools/neverd-gui/app/GnomeWindowEdges.cpp
/// How far inside an edge a press still resizes the window.
inline constexpr int kGnomeWindowEdgesEdgeWidth = 4;

// tools/neverd-gui/app/GraphView.cpp
inline constexpr double kGraphViewNodePadding = 6;
inline constexpr double kGraphViewWholeGraphExtent = 9e8;
inline constexpr qreal kGraphViewWheelZoomStep = 1.15;
inline constexpr qreal kGraphViewArrowLength = 9.0;
inline constexpr qreal kGraphViewArrowHalfWidth = 4.5;
/// How near an edge's line, in screen pixels, a click selects it and a hover
/// shows its other end.
inline constexpr qreal kGraphViewEdgeReachPixels = 4.0;
/// The rows an edge's hover shows of the block at its other end; a longer
/// block shows its first and last rows.
inline constexpr int kGraphViewEdgeHintRows = 12;

// tools/neverd-gui/app/HexView.cpp
inline constexpr int kHexViewMargin = 6;

// tools/neverd-gui/app/HighlightLocks.cpp
/// The colors IDA's Lock highlight offers, one word each.
inline constexpr int kHighlightLockColors = 8;

// tools/neverd-gui/app/JumpDialog.cpp
/// The dialog's first size: the matches beside a preview of where Enter goes.
inline constexpr int kJumpDialogWidth = 980;
inline constexpr int kJumpDialogHeight = 480;
/// The match list's first width; the preview takes the rest.
inline constexpr int kJumpDialogListWidth = 320;

// tools/neverd-gui/app/WheelInput.cpp
inline constexpr int kTextViewWheelLinesPerNotch = 3;

// tools/neverd-gui/app/ListingView.cpp
inline constexpr int kListingViewPrefetchMargin = 80;
inline constexpr int kListingViewTextMargin = 6;

// tools/neverd-gui/app/LoadFileDialog.cpp
// Room for the file's path and the processors' names.
inline constexpr int kLoadFileDialogDialogWidth = 560;

// tools/neverd-gui/app/LoadQuestionDialog.cpp
// The width of the PDB question, which these sit beside.
inline constexpr int kLoadQuestionDialogDialogWidth = 480;
/// The dialog's margins, the space between its lines and before its
/// buttons, on the 4-pixel grid.
inline constexpr int kLoadQuestionDialogMargin = 16;
inline constexpr int kLoadQuestionDialogLineSpacing = 4;
inline constexpr int kLoadQuestionDialogGroupSpacing = 8;

// tools/neverd-gui/app/MainWindow.cpp
inline constexpr int kMainWindowFunctionsWidth = 380;
inline constexpr int kMainWindowOutputHeight = 190;
inline constexpr int kMainWindowOverviewHeight = 170;
// Input dialogs leave room for a full name, comment or expression.
inline constexpr int kMainWindowInputDialogWidth = 420;
inline constexpr QSize kLocationChooserDialogSize{720, 480};
/// The command palette; dropped from the command center, it is at least as
/// wide as the field.
inline constexpr QSize kCommandPaletteSize{560, 420};
inline constexpr QSize kShortcutsDialogSize{800, 560};
inline constexpr QSize kExportDataDialogSize{760, 520};

// tools/neverd-gui/app/NavigationBand.cpp
inline constexpr int kNavigationBandBandHeight = 16;
inline constexpr int kNavigationBandLegendHeight = 18;
inline constexpr int kNavigationBandMargin = 4;
// The legend keys the band with small rounded swatches.
inline constexpr int kNavigationBandSwatchSize = 8;
inline constexpr qreal kNavigationBandSwatchRadius = 2;
inline constexpr int kNavigationBandSwatchGap = 6;
inline constexpr int kNavigationBandLegendEntryGap = 16;

// tools/neverd-gui/app/PdbPromptDialog.cpp
// Room for the question and a path of usual length.
inline constexpr int kPdbPromptDialogDialogWidth = 480;
/// The dialog's margins, the space between the lines of a group and the
/// space between groups, on the 4-pixel grid.
inline constexpr int kPdbPromptDialogMargin = 16;
inline constexpr int kPdbPromptDialogLineSpacing = 4;
inline constexpr int kPdbPromptDialogGroupSpacing = 8;
/// A slim bar, as Visual Studio Code draws progress.
inline constexpr int kPdbPromptDialogProgressHeight = 4;

// tools/neverd-gui/app/QuickStartDialog.cpp
/// The side pane: the product, above its ways to start.
inline constexpr int kQuickStartDialogSideWidth = 300;
inline constexpr int kQuickStartDialogSideTop = 32;
inline constexpr int kQuickStartDialogLogoSize = 56;
inline constexpr int kQuickStartDialogActionHeight = 56;
inline constexpr int kQuickStartDialogActionGlyphSlot = 36;
inline constexpr int kQuickStartDialogGlyphSize = 24;
/// Around an action's glyph, text and key cap.
inline constexpr int kQuickStartDialogActionPadding = 12;
inline constexpr qreal kQuickStartDialogRowRadius = 5;
/// The recent files.
inline constexpr int kQuickStartDialogRecentRowHeight = 60;
inline constexpr int kQuickStartDialogBadgeSize = 36;
inline constexpr int kQuickStartDialogEmptyGlyphSize = 44;
inline constexpr int kQuickStartDialogEmptyTextWidth = 300;
inline constexpr int kQuickStartDialogDropGlyphSize = 40;
inline constexpr int kQuickStartDialogDropInset = 14;

// tools/neverd-gui/app/SecondaryTextDelegate.cpp
// Room between an item's text and its secondary text, and after the latter.
inline constexpr int kSecondaryTextDelegateSecondaryGap = 16;
inline constexpr int kSecondaryTextDelegateSecondaryMargin = 8;

// tools/neverd-gui/app/TitleBar.cpp
// Shared title-bar geometry in logical pixels on every desktop.
inline constexpr int kTitleBarHeight = 35;
inline constexpr int kTitleBarIconSlot = 35;
inline constexpr int kTitleBarIconSize = 16;
inline constexpr int kTitleBarButtonWidth = 46;
/// The window controls draw their glyphs in a square of this side.
inline constexpr int kTitleBarGlyphSize = 10;
inline constexpr int kTitleBarCenterHeight = 22;
/// The command center takes this share of the bar's width.
inline constexpr int kTitleBarCenterWidthPercent = 38;
inline constexpr int kTitleBarCenterRadius = 6;
inline constexpr int kTitleBarCenterPadding = 8;
/// The magnifier's square and the space after it.
inline constexpr int kTitleBarLensSize = 12;
inline constexpr int kTitleBarLensGap = 6;
/// The room kept clear between the menus, the command center and the
/// buttons.
inline constexpr int kTitleBarGap = 8;
/// A dialog's title keeps this far from the bar's edge and the buttons.
inline constexpr int kTitleBarTitlePadding = 12;

// tools/neverd-gui/app/WindowFrame.cpp
/// The band outside the visible window that holds its shadow.
inline constexpr int kWindowFrameShadowExtent = 10;
/// Resize presses take precedence over content and docking inside this band.
inline constexpr int kWindowFrameResizeInset = 8;
/// Corner resize reach measured from the visible edge.
inline constexpr int kWindowFrameCornerReach = 16;
/// Bound a full-screen exit's wait if the user changed Dock/display settings.
inline constexpr int kWindowFrameScreenRestoreTimeoutMs = 1000;

// tools/neverd-gui/app/WorkbenchStyle.cpp
/// The size of icons drawn on the 16 pixel grid of IconSet.def.
inline constexpr int kWorkbenchStyleSmallIconSize = 16;
/// Qt's own file and font dialogs open at least this large, room to read a
/// folder's details or a list of fonts.
inline constexpr QSize kWorkbenchStyleFileDialogSize{860, 540};
inline constexpr QSize kWorkbenchStyleFontDialogSize{600, 460};
/// The widest a file dialog's size, type or date column fits its contents,
/// so that one long type cannot crowd out the names.
inline constexpr int kWorkbenchStyleFileColumnMaximumWidth = 200;

inline constexpr QSize kQuickStartDialogSize{900, 560};
inline constexpr QSize kQuickStartMinimumDialogSize{720, 460};
inline constexpr int kDefaultArrayElementBytes = 1;
inline constexpr int kDefaultArrayElements = 16;
inline constexpr int kDefaultDataAlignment = 16;
} // namespace neverd::limits::gui_config

#endif // NEVERD_TOOLS_NEVERD_GUI_CONFIG_H
