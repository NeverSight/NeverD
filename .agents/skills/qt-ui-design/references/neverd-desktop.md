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
- Let a primary button that cannot act fade, as Visual Studio Code's does, to
  40% (`ButtonPrimaryDisabled`, `ButtonPrimaryDisabledText`). The full accent
  behind gray text read as an enabled button that had broken.
- Recolor what a theme change cannot reach. Content that stores colors, such
  as the output log's text formats, keeps each part's level or role and colors
  it again on `Theme::changed`; the dark theme's gray log lines all but
  vanished on the light theme's white.
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
   100% and a fractional scale such as 150%. At 100%, thin lines and icon
   strokes must cover whole pixels (see "Crisp lines on the pixel grid").
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
