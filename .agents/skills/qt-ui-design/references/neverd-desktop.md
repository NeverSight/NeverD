# NeverD desktop design profile

Use this profile for NeverD's Qt desktop UI. It captures decisions established
through the Quick Start iterations and the workbench refinement pass, including
user feedback and actual Qt renders. These are product defaults, not a
universal definition of good taste.
Carry forward choices the user has accepted; change them when new evidence or
an explicit request warrants it.

## Visual direction

Aim for a calm, precise desktop analysis tool with clear, readable content.
Build refinement through proportion, alignment, typography, and coherent states.

- Use solid neutral surfaces with small differences between adjacent regions.
  Decorative gradients, logo halos, edge fades, and floating-card shadows do
  not belong in the established Quick Start direction. Preserve approved brand
  artwork; a logo's own colors are distinct from effects added around it.
- Keep a clear accent. Making every icon desaturated gray made the screen feel
  lifeless in user review. The accepted direction uses a readable blue shared
  by action glyphs and file outlines, with a modest emphasis for selection.
  Use semantic roles in `ChromeColors.def`, including light and dark values;
  treat the current palette as a reference, not a reason to recolor other views.
- Identify file formats with labels and shapes. ELF, BIN, and other formats
  do not need separate brightly colored tiles. Missing files retain a disabled
  treatment so the same accent does not imply availability.
- Distinguish selection, hover, press, and keyboard focus. Use a restrained row
  fill for the default action or selected file. A thin, visible neutral keyboard
  focus outline is appropriate; a persistent bright blue frame is too dominant
  for these small action glyphs. Keep focus reachable and perceivable.

## Workbench chrome

The workbench keeps IDA's layout, menus, and shortcuts with Visual Studio
Code's Dark+ and Light+ colors. When asked for more refinement, the user wanted
the overall style unchanged, so refine within it instead of restyling it, and
judge each change in the real window.

- Treat the accent as a state color. Selection, keyboard focus, the current
  position, and progress may use it; chrome surfaces may not. A full-width
  accent status bar read as decoration, so the status bar shares the windows'
  surface below a hairline, as in Visual Studio Code's Modern themes
  (`StatusBar`, `StatusBarText`, `StatusBarBorder`).
- Let the chrome read as few surfaces. The menu bar shares the toolbar's
  surface, so the band above the windows reads as one piece. Separate regions
  with 1-pixel hairlines (`Separator`, `StatusBarBorder`) instead of boxes,
  raised panels, or shadows.
- Quiet secondary text with color, not with a smaller size. Status lines,
  legends, and counts that describe rather than act use `SecondaryText`. Build
  a view's status line with `makeStatusLine()` in `ShrinkableRow.h`, so its
  margins and color match the other views.
- Show a control when it helps. A dock tab shows its close button only while it
  is current or hovered, and the hidden button keeps its space so the tabs do
  not shift (`DockTabBar` in `Docking.cpp`). Access-key underlines appear only
  while Alt is held or a menu opened from the keyboard is showing
  (`WorkbenchStyle`), as on Windows and GNOME.
- Let a toggle's glyph carry its state when the glyph can change. A
  disclosure chevron points where its panel goes, and a padlock closes and
  opens (`lock`, `lock_open`). Such buttons set the `glyphToggle` property,
  which keeps the checked accent fill off them, even while pressed: the user
  found an accent-filled lock in every floating caption too loud. Keep the
  fill for toggles whose glyph stays the same.
- Give popups room and a soft shape. Menus have a 4-pixel inset, item
  highlights with a 3-pixel radius, and separators inset from the menu's
  edges; tooltips pad their text. Keep radii small and consistent, and do not
  round docks or views. A combo box's list drops below it and reads as a menu
  (`combobox-popup: 0`, and the styled item delegate `WorkbenchStyle` gives
  every combo box so the style sheet's item rules apply).
- Keep a menu icon close to its label: NeverD's 16-pixel icon column uses
  about 8 logical pixels of separation, with text aligned across icon,
  checkmark and plain items. Qt already reserves the widest icon/check
  column; do not add another column in `QMenu::item` padding. Position the
  icon and checkmark in that same column, and reserve an icon's checked
  border while unchecked so toggling it does not shift the glyph. Check
  actual renders with shortcuts and submenus in both themes. This follows
  [Fluent's proximity and spacing guidance](https://fluent2.microsoft.design/layout)
  and [VS Code's aligned menu columns](https://github.com/microsoft/vscode/blob/main/src/vs/base/browser/ui/menu/menu.ts),
  while retaining NeverD's desktop density and colored icons.
- Keep menus taller than the available screen in one scrolling column.
  `WorkbenchStyle::SH_Menu_Scrollable` enables Qt's top/bottom scrollers,
  wheel input and keyboard navigation on every desktop. Fusion's default
  instead wraps overflow into additional columns. Use the shared style for
  main menus, context menus and submenus; do not split actions manually or
  impose a fixed menu height. Check an actual overflowing popup, including
  arrow interaction, wheel direction and keyboard access to its last item.
- Keep one comment command labelled **Enter comment...**, with `;` and `/`
  as equivalent shortcuts. Do not restore the separate ordinary-comment
  action or its `:` key. Scope these
  keys to analysis views so text fields retain normal punctuation input;
  keep only the primary key `;` in menus. The user rejected the crowded
  `;, /` menu hint. Show both bindings in tooltips, shortcut help and the
  command palette, and check aliases for conflicts. Let Qt render the primary
  menu shortcut; do not append aliases to the action's label.
- Keep code-window generation separate from Tab navigation. Local IDA 9.4
  verification showed that F5 from disassembly creates another pseudocode
  window, even for the same function, while F5 inside pseudocode refreshes
  that window. Tab reuses a code window. In NeverD, preserve the last code
  pane for Tab round trips; use the same generate/refresh rule for MedIR and
  the other representations. Additional documents become tabs and may float
  independently. Resolve the function from the originating code pane when
  it differs from the background disassembly. Check pending jumps, independent
  contents, language choices, detached panes, saved layouts and project close
  across the shared Qt controller, without platform-specific key branches.
- Open the command palette where it was asked for. A click on the title bar's
  command center drops it from the bar's bottom edge, centered under the field
  and at least as wide (`TitleBar::clickedCommandCenter`), as in Visual Studio
  Code. Its key and menu item keep opening it over the pane its commands act
  on; focus mode, whose caption has no command center, relies on that.
- Draw every glyph from `IconSet.def`. Arrows are its chevrons; message boxes,
  file dialogs and their tool buttons take its icons through
  `WorkbenchStyle::standardIcon` and `fileIcons()`, not the platform icon
  theme's.
- Keep categorical colors related. The navigation band's dark colors were
  harmonized in OKLCH: each category keeps its hue while lightness and chroma
  stay within one band, and the regular-function color no longer reuses the
  accent. Row
  tints for thunks and library functions carry about 40% less chroma than
  their earlier values, so they mark rows without competing with the text.
  Legend swatches are 8-pixel squares with a 2-pixel radius.
- The workbench draws its own title bar, after Visual Studio Code's custom
  title bar (`TitleBar`, `WindowFrame`): the menus sit in a 35-pixel title bar
  on the menu bar's surface, a command center names the open file and opens
  the command palette, and the 46-pixel window controls take a light wash on
  hover with red behind Close. The bar dims while the window is inactive.
  Dialogs take a title bar of their own, with the title in the middle and only
  the buttons their window flags ask for (`WindowFrame::frameDialogs`). Draw
  the bar's glyphs and the Wayland shadow from `ChromeColors.def` roles
  instead of borrowing the platform's.
- Use the application's frame, menus, controls and shadow on all desktops;
  native APIs still provide movement, resizing and window-state transitions.
  On Windows, NeverD draws its own frameless edges and shadow and disables
  DWM's non-client rendering; verify snapping, resizing, maximize and full
  screen with the native platform plugin. Read the
  [Windows desktop notes](windows.md) for transparent native-handle setup and
  desktop composition checks.
  Cocoa hosts a frameless window, and the platform hook disables its native
  shadow. Each platform's part lives under `app/platform`
  (`WindowFramePlatform.h`). For macOS input and validation details, read
  [macOS Qt rendering and input](macos-qt.md).
- Keep the signature elements. The toolbar's colored duotone icons stay: the
  user kept them over a single-accent recolor and an outline variant, and had
  earlier rejected all-gray icons as lifeless. Refine spacing, weight, and
  states around them instead of recoloring them.

Use Visual Studio Code as a reference for technique, not as a palette to copy
wholesale. The installed build keeps its themes in
`/usr/share/code/resources/app/extensions/theme-defaults/themes/`;
`dark_modern.json` and `2026-dark.json` show how it separates surfaces with
borders, quiets secondary text, and reserves the accent. Give every new color
a role in `ChromeColors.def` or `ThemeColors.def` with light and dark values;
style sheets and paint code read roles, never color literals.

## Lists, fields, and states

A review on 2026-10-25 measured these in captures of the real window; each
looked unfinished to the user once pointed out.

- Keep item text still. Horizontal padding in a `::item` rule applies only to
  the items the style sheet paints, the hovered and selected ones, so their
  text moved 2 pixels under the pointer. Item rules pad vertically only, and
  Qt's own text margin insets the text. A rule that makes every state drawable
  (a border or background on `::item`) also stops the base style painting
  `Qt::BackgroundRole`, which the thunk and library rows use.
- Draw nothing where two scroll bars meet. Fusion's corner panel left an
  L-shaped outline beside every list with both bars;
  `QAbstractScrollArea::corner` is transparent, so the view shows through as
  it does along the bars.
- Separate neighbors of nearly the same tone. A list's quick filter
  (`chooserFilter`) keeps 4 pixels below the list: the horizontal scroll bar's
  slider (`ScrollbarSlider`) and the field (`Input`) otherwise merged into one
  gray block. The field's edge lines up with the status line's text below.
- Recolor what a theme change cannot reach. Content that stores colors, such
  as the output log's text formats, keeps each part's level or role and colors
  it again on `Theme::changed`; the dark theme's gray log lines all but
  vanished on the light theme's white.
- Follow the desktop when the person chooses it. The System theme
  (`Theme::Choice::System`) takes `QStyleHints::colorScheme()` and follows
  `colorSchemeChanged`, as Visual Studio Code's
  `window.autoDetectColorScheme` does; an unknown scheme keeps the theme shown,
  and a theme chosen by name ignores the desktop. Paint code reads
  `Theme::mode()`, the palette shown, never the choice.
- Keep the high contrast themes Visual Studio Code's. Dark and Light High
  Contrast take their values from its `hc_black.json` and `hc_light.json`
  and from the `hcDark` and `hcLight` defaults of its color registry
  (`registerColor` calls in `workbench.desktop.main.js`, whose defaults refer
  to other colors and transform them). A registry color is `#RRGGBBAA`; Qt
  reads eight digits as `#AARRGGBB`, so convert before writing a `.def` entry.
- Where a high contrast theme outlines what the others fill (a hovered button
  or tool button, a menu or menu bar selection, a list selection, the current
  line, the word under the cursor), give it a border token that is
  transparent in Dark+ and Light+, and have the face or view always paint it;
  a fill or text color it changes gets its own token that repeats the old
  value in Dark+ and Light+ (`ListSelection`, `MenuSelectionText`,
  `ToolButtonChecked`). No `if (highContrast)` in paint code, and the normal
  themes paint as before. Rules that only the high contrast themes need live
  in `theme-high-contrast.qss`, appended to the style sheet in those themes;
  an item that can take an outline keeps a transparent one at rest, and a
  tree row's cells give up the sides they share (`:first`, `:middle`,
  `:last`) so the row takes one outline. A selection that fills with the text
  color needs its own text color (`ListingSelectionText`), and a highlight
  behind syntax colors (`paintHighlight`) outlines rather than fills.
- The System theme switches to the high contrast theme of the desktop's
  brightness when Windows runs a High Contrast theme or macOS has Increase
  Contrast on (`platform::desktopContrast()`), as Visual Studio Code's
  `window.autoDetectHighContrast` does. Qt reports an unknown color scheme and
  a theme change when Windows' High Contrast turns on; on Linux a person
  chooses a high contrast theme by name.
- `AppearanceTests::highContrastThemesReadClearly` holds the high contrast
  themes to WCAG's contrast: text at 7:1, secondary text and syntax at 4.5:1,
  lines and outlines at 3:1, translucent colors composited over their surface.
- Scroll sideways rather than cut content off. A view that paints rows of a
  fixed width, as the hex view does, scrolls horizontally when its pane is
  narrower than a row, keeps the byte the keys reach in view, and maps clicks
  through the offset.
- Give Qt's own dialogs the room the workbench's dialogs have. The file dialog
  opens at 860 by 540 with its size, type and date columns fitted to the folder
  shown and the name taking the rest; Qt measures those columns from "128.88
  GB" and "mp3Folder" and elides "10.02 MiB" and "Windows or EFI program". The
  font dialog opens at 600 by 460, where its lists show more than three rows.
  `WorkbenchStyle` applies both as the dialogs show.
- Return in a dialog presses its default button on every platform, also from
  a check box, a radio button, or a combo box that takes no typing
  (`DialogReturnKey`). Qt's GNOME theme otherwise makes Return toggle the box
  or reopen the list, unlike Windows and macOS.

The user kept two choices that a review may question: the load dialog centers
OK and Cancel as IDA does, and the Functions list keeps its horizontal scroll
bar for the columns beyond its pane.

## Control states

A review on 2026-11-09 rendered every common control at rest, under the
pointer, pressed, with keyboard focus, checked and disabled, in both themes.
Each state must read differently from the others, and from both themes at
once: the user called the disabled buttons "lifeless", and found a disabled
combo box that looked enabled. Take each value from a primary source, as the
rules below do, not from taste.

- A button or field that cannot act recedes to a faint plate under a dimmed
  label (`ControlDisabled`, `ControlDisabledBorder`, `DisabledText`): the
  button gray at 40%, as Visual Studio Code's `.monaco-button.disabled` fades
  the whole button, with no edge in the dark theme and a 4% edge in the light
  one, whose plate alone lies within a twentieth of the window. The plate
  must be there, and nothing of the control may stand out from its surface by
  more than a quarter: Windows 11 composites to 1.14:1 for the plate and
  1.23:1 for the edge (`ControlFillColorDisabled` inside
  `ControlStrokeColorDefault`, over `#202020`), and Visual Studio Code's
  secondary button at 40% to 1.04:1 and 1.19:1 in Dark+, or to an edge of
  1.13:1 alone in Dark Modern, whose enabled button is an outline too. Two
  earlier looks failed with the user. A full
  `Button` fill behind gray text turned Binary comparison, whose actions wait
  for input, into a wall of dead gray blocks ("lifeless"). An outline alone
  in the full `Border` color was as loud as an enabled button's fill (1.39:1
  beside 1.40:1) around nothing, and the same dialog became a wall of empty
  frames ("a bit ugly"); the note here that it matched Windows 11 was wrong
  for the dark theme, where WinUI keeps its plate. Check boxes and radio
  buttons keep `DisabledBorder`: a small box needs its outline. Combo box and
  spin box chevrons dim with the field (`chevron_*_disabled-<mode>.svg`), and
  a disabled partly checked box matches a disabled checked one.
- Give every control the style sheet colors a `:disabled` rule. An explicit
  `color` or `background` hides the palette's disabled colors, so a disabled
  combo box, such as Binary comparison's SSA representation, looked exactly
  like an enabled one, and labels, group titles, tool buttons, tabs and lists
  kept full-strength text.
- A primary button that cannot act keeps a faint accent, so a dialog's
  default action stays recognizable. The dark theme draws the button at 40%,
  as Visual Studio Code's `.monaco-button.disabled` does
  (`ButtonPrimaryDisabled`, `ButtonPrimaryDisabledText`); in the light theme
  white at 40% vanished into the pale fill (1.3:1), so there the fill is the
  accent at 18% and the label the window text at 55% (2.7:1). The full accent
  behind gray text read as an enabled button that had broken.
- A checked tool button that cannot act keeps that plate in place of its
  fill; a toggle whose glyph shows its state keeps neither.
- Do not show commands that cannot apply yet as a block of disabled buttons.
  Hide by mode, disable by selection, as the Windows guidelines put it:
  Binary comparison's eleven commands on a pair appear with the first
  comparison, before which there is no function to select, and from then on
  each disables in place. Group a row of commands by what they act on, with
  a wider gap between groups than within one, at their natural widths in a
  `FlowLayout` that wraps a group whole; a grid stretched them to its columns
  and scattered them.
- Give a window at most one default action, and only while it is the next
  step. Qt makes a dialog's first push button its default and then each
  button the keyboard reaches, so Binary comparison drew Browse in the accent
  and Return in its filter pressed it. Turn `autoDefault` off for a tool
  window's buttons, as Lineage does, and set the default yourself: Compare
  until a comparison exists and none after, when Return in the filter would
  start another. Such a window must then answer Return itself. `QDialog`
  presses the first default button among all its descendants, those of an
  open child window included, and a button without `autoDefault` passes the
  key up to it. Binary comparison presses the button that holds focus,
  compares from an input field only where the inputs differ from the
  comparison shown, and otherwise swallows the key.
- A spin box's arrow dims where its range ends. Qt marks that arrow `:off`,
  from `stepEnabled`, not `:disabled`.
- Make hover visible. `ToolBarHover` sat within two levels of the dark
  `Button`, so buttons seemed not to respond; `ButtonHover` is Visual Studio
  Code's `secondaryHoverBackground` (`#45494E` Dark+, `#CCCCCC` Light Modern).
  A disabled control never shows hover: Qt's style sheet sets `:hover` only
  for enabled widgets, and the `:disabled` rules follow the `:hover` ones.
- Frame keyboard focus around the whole control in the `Focus` color, the
  color of the fields' focus border, as Visual Studio Code draws its
  `focusBorder`. `WorkbenchStyle` draws `PE_FrameFocusRect` as that frame: a
  button's face, a check box's or radio button's box with its label, a tab,
  a dock's tab. A push button keeps a line of the surface it stands on between
  the frame and its face, as Visual Studio Code sets its focus border off a
  button, so the frame shows on the primary accent too. That line takes the
  color of the nearest ancestor that paints an opaque background (a style
  sheet's `background` reaches that widget's palette), not the window's: on a
  `Base` panel a window-colored line read as a second, wrong outline.
  Fusion's rectangle tinted a box behind the label, which read as selected
  text in the light theme and vanished in the dark one. Focus the pointer
  gives draws none, as in Fusion (`State_KeyboardFocusChange`). Qt paints a
  tool button whose style sheet gives it a border without calling
  `PE_FrameFocusRect` (`CC_ToolButton` in `qstylesheetstyle.cpp`); a tool
  button takes focus only from the keyboard (`Qt::TabFocus`), so
  `QToolButton:focus` draws the frame as its border. Quick start's rows draw
  the same frame themselves.
- Size a dialog's buttons as a row of equals: at least 75 pixels, the
  standard Windows command button (Fluent 2's buttons start at 64 and 96).
  `QDialogButtonBox QPushButton { min-width: 45px; }` is that width less the
  padding and border. OK was 48 pixels beside a 68 pixel Cancel.
- Start every column a person can resize at least as wide as its heading,
  and end a heading that later runs out of room in an ellipsis. Qt's header
  defaults to `Qt::ElideNone`, so a centered heading lost both ends ("imary
  addre"). `WorkbenchStyle` sets `Qt::ElideRight` on every header and gives
  each horizontal one a `HeadingFitter`, which widens an interactive column
  to its `sectionSizeHint` when the header first shows, when its columns or
  headings change and when the language changes; a person can still narrow
  it. Starting widths written for English cut "Architectural state" in x87
  support and "Analyst disposition" in Offline package review, and a live
  switch to Japanese cut the Functions list's "Segment".
- A button that opens a list shows the chevron a combo box draws, inside its
  own padding (`QPushButton::menu-indicator`), not Fusion's corner triangle,
  which Edit function's Color and Binary comparison's Regions showed.
- Dim a menu item's shortcut and submenu chevron to 70% until the item is
  highlighted, as Visual Studio Code's `menu.ts` dims a keybinding; a disabled
  item's shortcut is already `DisabledText`. A menu the style sheet paints
  draws label and shortcut with one pen (`CE_MenuItem` in
  `qstylesheetstyle.cpp`), so `WorkbenchStyle` paints the items: the `QMenu`
  rule carries `-qt-style-features: background-color`, which lets the base
  style paint over the style sheet's background, and keeps no `QMenu::item`,
  `::icon`, `::indicator` or `::right-arrow` rule, any of which hands the items
  back to the style sheet. The geometry is the style sheet's that came before:
  padding 8, 24 and 4 pixels, an 18 pixel icon box 3 pixels in (the style
  sheet counts a sub-control's border in its size), a 16 pixel check mark 4
  pixels in and a 10 pixel chevron. Right to left, the chevron points left;
  the style sheet had fallen back to Fusion's triangle there.

## Judging a look

The user judges NeverD's windows by comfort ("按照最舒服的来") and names a
defect by how it feels, not by its cause. Each verdict below was traced to a
number or a count; start from these before reaching for taste.

| Verdict | What the window showed | Cause, measured | What settled it |
| --- | --- | --- | --- |
| "lifeless" (2026-11-09) | Disabled buttons as full gray blocks | The enabled fill (1.40:1 on the dark window) kept behind a gray label | The fill must fade with the label |
| "有点难看" (2026-11-12) | Disabled buttons as hollow outlines | An outline of 1.39:1 around nothing, as loud as an enabled fill; seventeen at once | A faint plate (1.14:1), and commands that cannot apply yet stay away |
| "太左了" (2026-11-12) | A pane flush against the combo box beside it | A 6-pixel splitter handle painted whole in the fields' gray | A hairline in a 13-pixel gutter, first rows on one line |

How loud a button that cannot act is, as the contrast of each part against
what lies behind it, composited over each system's own surface (fetched
2026-11-12; `-` is not drawn):

| Dark theme | Plate | Edge | Label |
| --- | --- | --- | --- |
| Windows 11 (WinUI 3) | 1.14 | 1.23 | 3.25 |
| Visual Studio Code, default secondary at 40% | 1.04 | 1.19 | 2.65 |
| Visual Studio Code Dark Modern, secondary at 40% | - | 1.13 | 2.82 |
| Fluent 2 web (a plate darker than the surface) | 1.27 | 1.45 | 2.76 |
| NeverD until 2026-11-12 | - | 1.39 | 3.00 |
| NeverD now | 1.14 | - | 2.64 |
| NeverD's enabled button, for scale | 1.40 | 1.39 | 6.80 |

| Light theme | Plate | Edge | Label |
| --- | --- | --- | --- |
| Windows 11 (WinUI 3) | 1.02 | 1.11 | 2.49 |
| Visual Studio Code Light Modern, secondary at 40% | 1.07 | 1.19 | 1.99 |
| Fluent 2 web | 1.14 | 1.32 | 1.65 |
| NeverD until 2026-11-12 | - | 1.14 | 2.36 |
| NeverD now | 1.05 | 1.11 | 2.25 |
| NeverD's enabled button, for scale | 1.11 | 1.14 | 9.06 |

- Trace a verdict to its cause before changing a color. Measure the parts of
  what the user points at, and count how many of it the window shows. The
  hollow buttons were a token defect (the outline's strength) and a
  structure defect (seventeen commands waiting) at once, and the earlier
  token-only fix had only traded one verdict for the next.
- Offer the choice as renders of the real window, switched in place: the
  window as it is, the token change alone, and the token change with the
  structure change. The user left the choice to comfort; the regrouped
  window was taken because it opened quietest while its buttons still looked
  like buttons.
- Keep faces on text commands. A row of flat text commands, as Visual Studio
  Code and Windows command bars draw them, was rendered and set aside: without
  icons the commands read as labels. Revisit it once the commands have icons;
  the icon set has `check`, `link`, `refresh` and `pathfinder`, and none for
  unmatching a pair.
- Let the accent say what comes next. In Binary comparison it sits on Compare
  until a comparison exists, faintly while Compare cannot act yet, and on
  nothing afterwards.
- Look along every seam. Two shapes of one gray that meet read as one shape:
  the splitter's band and the combo box beside it were both `#3C3C3C`. Leave
  a layout's spacing between a control and any dividing line, and put the
  first rows of two panes on one line so the rows below start level.
- Render at the desktop's own font size. The user's GNOME desktop gives the
  interface about 11 points where Qt's offscreen platform gives 9: the same
  Cancel button is 77 by 31 pixels on their screen and 68 by 27 offscreen,
  and a row that fits offscreen wraps for them. Set the application font's
  point size in the throwaway driver; `QT_FONT_DPI` scales everything, not
  the font.

## Hierarchy and proportion

Start by identifying the user's task and the order in which information should
be read. Inspect the whole window before adjusting individual colors.

- Give filenames and action titles priority. Quick Start gives the filename a
  full line, then places the folder and last-opened time below it. Metadata
  should not consume the space needed to distinguish two filenames.
- For long binary names, preserve both ends when eliding so suffixes, build
  variants, and extensions remain useful. Provide the full path and time on
  hover. Keep the primary information usable without requiring hover.
- Keep branding subordinate to starting work. Inspect the combined weight of
  the logo, title, tagline, and version; several individually modest elements
  can still dominate as a group. Startup preferences are secondary controls.
- Use the platform's GeneralFont reading size for both interface and code,
  retaining NeverD's bundled faces. Keep user overrides and avoid a
  shared hard-coded point size or a second Retina multiplier. In the verified
  Quick Start, 14 logical pixels for row titles and 12 for supporting text fit
  the task; 16 pixels is not a mandatory minimum for every dense desktop row.
  Evaluate actual readability, contrast, DPI scaling, and larger system fonts.
  Do not force every size onto a mathematical ratio when that damages fit.
- Use a small spacing scale, generally based on 4 logical pixels. Account for
  SVG padding, stroke placement, and optical alignment instead of rounding
  blindly. Align text edges consistently and center icons within their slots.
  Give related title/description pairs less space than separate action groups.

## Icon family

Use NeverD's original SVGs and the icon registry. Match the icon family across
both columns: stroke weight, corner treatment, apparent size, and color roles.
A shared bounding box alone does not guarantee equal visual weight.

Quick Start uses line glyphs drawn on a 24-pixel grid, without individual rounded
background tiles, beside outlined file icons. Keep them crisp at normal and
fractional DPI, as the next section describes. File labels must fit their
outline, including longer labels such as MACH, NDDB, and WASM. Follow
`IconSet.def` for asset provenance and palette conventions; do not copy a
third-party icon set merely to imitate an IDE.

## Crisp lines on the pixel grid

The user reads a blurred thin stroke as a color problem. The Quick Start's blue
line glyphs looked heavy to the user because their 1.4-pixel strokes sat on
whole-pixel coordinates, so each stroke covered two pixels at about 70%; the
New glyph averaged `#4673A2` instead of its `#5DA9F6`. Snapping the strokes
fixed most of it, and a lighter icon blue did the rest. An audit of the whole
icon set then found the same fault in the file dialogs' icons and places pane,
the operand-type and function icons, the partly checked box, and the
navigation band's cursor.

- Draw every straight stroke where it covers whole pixels at the size the
  shape is drawn. A stroke at least 1 and under 2 pixels wide centers on a
  pixel center (`x.5`), a 2-pixel stroke centers on a pixel boundary, and the
  edge of a fill without a stroke lies on a pixel boundary. Curves and
  diagonals antialias anyway.
- Snap at the size Qt draws the asset: `IconSet.def` icons at 16 pixels
  (toolbars, menus, item views, file dialogs), Quick Start glyphs at 24, and
  style-sheet chrome at its subcontrol size (chevrons at 10, menu checks and
  check box marks at 14, toolbar grips at 6 by 16). A whole-number multiple
  keeps a snapped icon crisp, so message box icons, drawn at 32, stay crisp
  even where a 16-pixel source line is not. A size that is no whole multiple
  blurs any grid: Qt's file dialog drew its places pane at 24, so
  `WorkbenchStyle` draws it at 16 like the file list. Sizes that cannot be
  snapped, such as the 40-pixel drop glyph or a 150% scale, only need to stay
  no worse.
- A symmetric shape with a 1-pixel line on its axis cannot be both centered and
  crisp. Move it half a pixel off center, as `operand_negate` does, or give the
  axis line 2 pixels. The `signatures` fingerprint keeps its soft center ridge
  on purpose.
- In paint code, fill axis-aligned lines as whole-pixel `QRect`s, offset
  1-pixel pens by 0.5 in a `QRectF`, and round a computed position before
  drawing a thin line with antialiasing on, as the navigation band rounds its
  cursor. `GraphView` turns antialiasing off from 100% zoom up so node borders
  and edges stay sharp.
- In the dark theme, thin glyphs take a lighter tone than text of the same hue,
  as Visual Studio Code paints colored icons `#75BEFF` and links `#4DAAFC`.
  Judge a thin stroke's color in a capture at 100%, not by its hex value.
- `scripts/check_gui_icons.py` enforces these rules for every icon at its
  viewBox size, and the GUI workflow runs it. It reads the path geometry, so it
  sees inner lines over fills and ignores curves, whose extremes always
  antialias. `NEVERD_ICON_SOFT_LINE` in `IconSet.def` records an exception
  with its reason. To judge a single glyph, compare the share of its drawn
  pixels at full coverage in a capture before and after a change.

### Fractional display scales

Many Windows laptops run at 125%. There Qt places a child widget at a
fraction of a device pixel (a widget at logical x 26 starts at device 32.5)
and clips its painting to its sides rounded to the nearest pixel, half up. A
style sheet border is a 1.25-pixel antialiased rectangle at that fraction, so
it spreads over two pixels at partial strength, with a seam of the surface
between a rounded fill and its border at some offsets. Only the top edge of a
field looked sharp. Chrome boxes and hairlines do not have to look like this.

- Paint chrome boxes and hairlines through `DevicePixels` (`app/DevicePixels.h`):
  `paintDeviceBox` for a fill and border with round corners,
  `fillDeviceLine` for a separator, `paintDeviceSeparator` for a resize
  handle's hairline and highlight, and `DevicePixels::outline` for a pen
  outline such as a focus frame. It rounds each side to the nearest device
  pixel, as Qt rounds a widget's clip, so neighbors share their edges, and it
  draws a line as many whole device pixels as its scaled width holds, at
  least one, as a browser snaps a border. Where each logical pixel already
  covers whole device pixels (100%, 200%), it leaves the painter alone, so the
  output equals the style sheet's pixel for pixel;
  `AppearanceTests::deviceBoxesPaintAsStyleSheetsAtWholeScales` holds it to
  that.
- `WorkbenchStyle` paints the boxes the style sheet used to: fields, push and
  tool buttons, the combo box's list and the cards by object name, just
  before the widget paints (the application event filter's `QEvent::Paint`);
  check boxes, radio buttons and list items' boxes, group frames, menus,
  tooltips, headings, the status bar, splitter handles and menu separators
  through the primitives Qt hands to the base style; tabs whole. Their rules
  keep the box model with `border: 1px solid transparent`, or
  `border: 1px native` where Qt then delegates the box. The colors live in
  `WorkbenchStyle`'s face functions, with the state the style sheet's
  pseudo-states read (`paintedOption` asks the widget for its own option).
- Before moving a rule, read the cascade that applies to it, not the rule
  alone. `QAbstractScrollArea { border: none }` later in `theme.qss` takes the
  outline off text edits, so an editor shows an outline only with focus.
  `border: 1px native` with any `border-radius` becomes no border at all.
  Removing a widget's last `:hover` rule also drops the mouse tracking the
  style sheet turned on for it, which `QPushButton` needs to set its hover
  state, so `WorkbenchStyle::polish` turns it on. A rule that keeps a fill for
  boxes the style does not paint, such as the field rule's radius for
  read-only editors, must stay.
- Qt passes the splitter, not the handle, as `CE_Splitter`'s widget; a
  default-constructed `QSizeF` is -1 by -1, not empty.
- A splitter between panes that hold controls needs a gutter. The theme's
  handle is a hairline, 1 pixel that Qt lets the pointer grab over 5, which
  suits panes that are surfaces, such as two tables. Binary comparison set a
  6-pixel handle to keep its combo boxes off the next pane, and the style
  filled all of it: a band of the fields' own gray that the "All functions"
  box ran into, with the next pane's title flush against it ("太左了").
  `WorkbenchStyle` draws a handle's hairline along the middle of its room,
  under a dock separator's bar while the pointer is over it, so a wider
  handle is a gutter: a layout's spacing on either side of the line
  (`kPaneGutter`). Across such a gutter, put what the panes show
  first on one row (the details' title on the inventory's filters' row), so
  the rows below start level.
- Verify at 125%, 150% and 175% with
  `AppearanceTests::controlsKeepTheirLinesOnDevicePixels`: render the window
  with `QWidget::render` into a `QImage` whose device pixel ratio is the scale,
  at four logical offsets so the box lands on each quarter pixel, and require
  every pixel on a line across the box to be the surface, the border or the
  fill. Then compare captures at 100% before and after the change pixel for
  pixel; any difference there is a regression or needs a reason.

## Languages and direction

The workbench speaks eleven languages, Arabic right to left, and switches
between them while it runs. A review on 2026-11-09 opened every dialog command
in every language and compared every text of the main window across them.

- Lay out geometry you place by hand left to right, then mirror it with
  `QStyle::visualRect(layoutDirection(), rect(), r)`. The title bar draws its
  own controls, so in Arabic it stayed left to right until `TitleBar::arrange`
  mirrored it: the window controls lead on the left, the icon and the menus
  on the right. In a painter on a right-to-left widget `Qt::AlignLeft` means
  the right edge unless it is combined with `Qt::AlignAbsolute`; that pushed
  the command center's file name out of its box.
- Wrap a path that stands in right-to-left text in `leftToRight()`
  (`BidiText.h`, a left-to-right isolate): a Unix path starts with a neutral
  slash, which otherwise takes the paragraph's direction and moves to the
  path's end, as Quick start's recent folder read "mnt/data/" in Arabic.
- Measure a `QMenuBar` from its items, not from its `sizeHint()`. Right to
  left, it mirrors its items within its current width before measuring them
  (`QMenuBarPrivate::calcActionRects` in Qt 6.8), so a bar sized from its own
  hint got two pixels and hid every menu (`menusWidth` in `TitleBar.cpp`).
- Set again, when the language changes, every text a view writes while it is
  built: labels, tooltips, placeholders, headings, combo box items, tab and
  menu titles. Use `keepTranslated(owner, apply)`, which runs `apply` now and
  on each `QEvent::LanguageChange`, or the view's own `changeEvent`. Without
  it a live switch left the Pathfinder, Local types, Type libraries, Analysis
  queries, MCP connections and plugin panes, and the Plugins menu, in the
  language they started in. A widget another library builds needs the same:
  the docking title bar's buttons and `QTabBar`'s close buttons word their
  tooltips only as they are built.
- Put every translatable string in every catalog, the strings of `.def`
  tables and of libraries that ship no catalog included. The unwind dialog's
  21 strings, kept in `UnwindInfo.def`, were in no catalog, so the dialog was
  English in every language; the docking library's tooltips translate under
  its own contexts, `KDDockWidgets::QtWidgets::TitleBar` and
  `KDDockWidgets::Core::TitleBar`. Qt's own dialogs and key names use the
  `qtbase_<language>.qm` Qt ships. `LanguageSwitchTests` checks a few
  strings of each such source.
- Known limit: messages the analysis engine sends, such as why an operand has
  no structure offset, are English in every language.

## Resizing is part of the design

Content-heavy dialogs should support ordinary desktop resizing unless the task
requires fixed geometry. Set a useful initial size and a content-based minimum
instead of locking the whole dialog with `setFixedSize`.

- Resize from the window's edges and Qt's resize grip. A frameless Linux
  window resizes from every edge and corner, starting four pixels inside its
  visible edge, and under Wayland from its shadow band too. On Windows, check
  edge and corner gestures on the custom frame under the native desktop. Let
  additional width expose more content and additional height expose more rows.
  Stable navigation or action panes can retain their width while the content
  pane expands.
- Remember a user's chosen size where useful. Bound restored dimensions to
  the available screen, and save normal geometry when closing a maximized
  window. Avoid persisting a size that makes controls unreachable.
- Check the minimum, initial, and enlarged sizes. Text may elide; controls,
  shortcut hints, and hit targets must remain usable. Drag overlays, empty
  states, and keyboard focus must track the resized viewport.

## Verify rendered behavior

Use the real Qt implementation, fonts, and theme. A browser mockup or source
diff does not establish the appearance of a Qt widget.

1. Build the affected GUI targets and inspect real captures in both themes at
   100% and a fractional scale such as 125% or 150%. At 100%, thin lines and
   icon strokes must cover whole pixels, and at a fractional scale chrome
   borders and hairlines must still take whole device pixels (see "Crisp lines
   on the pixel grid").
   Check long names, empty content, disabled actions, selection, hover, and
   keyboard focus as relevant.
2. For resizing changes, exercise a mouse gesture, content expansion, minimum
   size, and reopening. Use isolated settings so previews and tests do not
   replace the user's preferences or recent-file history.
3. Inspect actual text and widget geometry. Integer font-metric rounding can
   cause a needless ellipsis; verify the final date or shortcut rather than
   assuming its measured width fits. Scrollbars may already be polished before
   an object name is assigned; repolish a scoped widget if its QSS rule does
   not take effect, and confirm the result in pixels.
4. Use existing theme/icon checks and focused interaction regressions. Add a
   test for observable behavior such as drag resizing, not for a preferred
   color literal or a screenshot's exact pixel arrangement.
5. Compare before and after under one layout. Capture both builds from copies
   of `NeverD.conf` and `desktop.json` in an isolated configuration directory,
   because the user's live layout changes between runs, and wait until slow
   regions such as the navigation band have painted.
6. Test window-manager behavior, such as edge resizing, maximizing, full
   screen, and dialog attachment, under a real compositor. On Linux, keep the
   desktop session's `XDG_CURRENT_DESKTOP` and `XDG_SESSION_TYPE`: Xvfb has no
   window manager, and without those variables Qt picks a different window
   decoration and the GNOME-specific code stays off. On Windows, use the
   native desktop checks in the [Windows desktop notes](windows.md).
7. Review without touching the user's desktop in a private headless GNOME
   Shell (`gnome-shell --headless --unsafe-mode --wayland` on its own D-Bus
   session, as `tools/neverd-gui/tests/run_wayland_docking.py` starts it),
   capturing through `org.gnome.Shell.Screenshot` and sending input through
   Clutter virtual devices from `org.gnome.Shell.Eval`. Measure a suspected
   shift in pixels, such as the first column of a hovered row's text against
   its neighbors', instead of judging it by eye.
8. Check control states in a state gallery: a throwaway `main` linked against
   the build's `libneverd_gui_app.a` that calls `Theme::instance().apply()`
   and grabs each control offscreen at rest, under the pointer
   (`QTest::mouseMove`), pressed, with keyboard focus
   (`Qt::WA_KeyboardFocusChange` on the window, then
   `setFocus(Qt::TabFocusReason)`), checked and disabled, in both themes.
   The headless shell never paints `:hover`; a private `Xvfb` display does,
   under a real pointer (`QT_QPA_PLATFORM=xcb`, `xdotool mousemove`, `xwd
   -root`), and never touches the user's desktop. A `QDialog` keeps a single
   `:default` button, so host the controls in a plain `QWidget`. Measure
   contrast from the rendered pixels, and compare before and after from the
   same gallery. The offscreen screen is 800 by 600, which narrows a
   `QMessageBox` (its width limit is the screen's width less 480 pixels);
   judge message boxes in the real session.
9. Survey languages with a throwaway replacement for
   `tests/WidgetsProbe.cpp`, linked into a private copy of `neverd-gui` and
   started with `--widgets-test` offscreen on a full-size screen
   (`QT_QPA_PLATFORM=offscreen:configfile=<json>` with a `screens` entry of
   1920 by 1080). For each language, call `applyLanguage`, trigger every
   dialog command, and for each window flag a label narrower than its
   `minimumSizeHint`, a button narrower than its `sizeHint`, a heading
   narrower than its `sectionSizeHint`, and a visible menu bar or toolbar
   extension button; save a capture. Log every text as well, the main
   window's hidden panes and menus included: a text equal to English in every
   other language is either missing from the catalogs or written once and
   never set again, and the catalogs tell which.

10. Render a dialog in the states that matter with a throwaway driver linked
    against the build's `libneverd_gui_app.a`: construct the real dialog,
    drive it through its states (as it opens, with its inputs chosen, with
    its results and a selection), and grab each in both themes. Give it the
    worker of a qualified bundle and private `XDG_*` directories, and private
    copies of its input files, since comparing writes sidecars beside them.
    Set the application font to the desktop's size first (see "Judging a
    look"). A dialog that sets `Qt::WA_DeleteOnClose` must not be closed
    from a stack-allocated object. To show the build before a change from
    the same build directory, link the driver with objects compiled from the
    old sources ahead of the archive.

When reporting an audit, connect each finding to its consequence and proposed
change: for example, a timestamp beside the filename hides a build suffix, so
move metadata below it. Separate observable defects from aesthetic judgment.
Show the actual result and state what was verified. Avoid promising objectively
perfect aesthetics or using decorative effects as a substitute for hierarchy.

## References and implementation

- [NN/g: visual design principles](https://www.nngroup.com/articles/principles-visual-design/)
  — scale, hierarchy, balance, contrast, and grouping.
- [Fluent 2: layout](https://fluent2.microsoft.design/layout)
  — spacing, optical alignment, and responsive content allocation.
- [Microsoft: alignment, margins, and padding](https://learn.microsoft.com/en-us/windows/apps/develop/ui/alignment-margin-padding)
  — fluid layout with deliberate minimum sizes and consistent gutters.
- [Microsoft: icon design](https://learn.microsoft.com/en-us/visualstudio/extensibility/ux-guidelines/images-and-icons-for-visual-studio#style-details)
  — simple metaphors and consistent visual weight across a family.
- [Qt: QDialog resize grip](https://doc.qt.io/qt-6/qdialog.html#sizeGripEnabled-prop).
- [Visual Studio Code: theme color reference](https://code.visualstudio.com/api/references/theme-color)
  — the roles behind its surfaces, borders, and state colors.
- Visual Studio Code's own sources for its states:
  [button.css](https://github.com/microsoft/vscode/blob/main/src/vs/base/browser/ui/button/button.css)
  (a disabled button at 40%),
  [menu.ts](https://github.com/microsoft/vscode/blob/main/src/vs/base/browser/ui/menu/menu.ts)
  (shortcuts at 70%), and the
  [Dark Modern](https://github.com/microsoft/vscode/blob/main/extensions/theme-defaults/themes/dark_modern.json)
  and [Light Modern](https://github.com/microsoft/vscode/blob/main/extensions/theme-defaults/themes/light_modern.json)
  button colors.
- [Windows guidelines: dialog boxes](https://learn.microsoft.com/en-us/windows/win32/uxguide/win-dialog-box#disabling-or-removing-controls-vs-giving-error-messages)
  (remove a control users do not expect to apply, disable one whose state
  changes often) and
  [command buttons](https://learn.microsoft.com/en-us/windows/win32/uxguide/ctrl-command-buttons)
  (the default button, at most two widths in a window).
- [Visual Studio Code: views](https://code.visualstudio.com/api/ux-guidelines/views)
  — buttons only for primary actions, and few of them.
- [WinUI theme resources](https://github.com/microsoft/microsoft-ui-xaml/tree/main/controls/dev/CommonStyles)
  — `Common_themeresources_any.xaml` and the Button, TextBox, ComboBox,
  CheckBox and ToggleButton resources give Windows 11's disabled fill,
  stroke and text for each control.
- [Fluent 2 button styles](https://github.com/microsoft/fluentui/blob/master/packages/react-components/react-button/library/src/components/Button/useButtonStyles.styles.ts)
  — sizes, minimum widths and state tokens.
- Qt 6.8 behavior the rules above rely on, in `qstylesheetstyle.cpp`
  (`pseudoClass()` sets `:hover` only for enabled widgets; `CE_MenuItem`
  draws a shortcut with its label's pen; `CC_ToolButton` paints a bordered
  tool button without a focus rectangle), `qmessagebox.cpp` (a message box
  is at most the screen's width less 480 pixels), `qheaderview.cpp`
  (headers default to `Qt::ElideNone`), `qfusionstyle.cpp`
  (`PE_FrameFocusRect` draws only with `State_KeyboardFocusChange`),
  `qmenubar.cpp` (`calcActionRects` mirrors each item within the bar's
  current width, which `sizeHint()` then measures) and
  `qoffscreenintegration.cpp` (the `configfile` platform option and its
  `screens` array).
- [CSS Color 4: OKLab and OKLCH](https://www.w3.org/TR/css-color-4/#ok-lab)
  — a perceptual space for related categorical colors.
- Local examples: [QuickStartDialog.cpp](../../../../tools/neverd-gui/app/QuickStartDialog.cpp),
  [ChromeColors.def](../../../../tools/neverd-gui/app/ChromeColors.def),
  [ThemeColors.def](../../../../tools/neverd-gui/app/ThemeColors.def),
  [theme.qss](../../../../tools/neverd-gui/resources/theme.qss),
  [IconSet.def](../../../../tools/neverd-gui/app/IconSet.def),
  [Docking.cpp](../../../../tools/neverd-gui/app/Docking.cpp),
  [WorkbenchStyle.cpp](../../../../tools/neverd-gui/app/WorkbenchStyle.cpp), and
  [GUI testing guidance](../../../../docs/testing.md#gui-function-lists-and-navigation).
