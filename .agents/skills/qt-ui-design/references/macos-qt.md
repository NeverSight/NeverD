# macOS Qt rendering and input

Use these checks when implementing or verifying Qt desktop UI on macOS. They
come from failures reproduced with NeverD's Qt Widgets workbench on Cocoa;
apply NeverD's appearance choices only when working on NeverD.

## Menus and icons

Qt 6.7.3 changed the macOS default of `Qt::AA_DontShowIconsInMenus` to true.
`QAction::setIcon()` can therefore load a valid icon that appears in a toolbar
but disappears in menus. This also affects dynamically created context menus.
For NeverD's application-painted menus, `Theme::apply()` clears this attribute
after `QApplication` has initialized platform defaults. Native-menu ownership
and icon visibility are separate choices; changing `setNativeMenuBar()` alone
does not restore icons.

Verify both `action->icon().pixmap(...).isNull()` and
`action->isIconVisibleInMenu()`, then inspect an actual menu. Include a dynamic
context menu such as Output's Copy/Select all/Clear, not just registry actions.
If icons are actually missing from a packaged application, inspect its resource
registration and SVG runtime separately from the visibility policy.

An unusually wide context menu can be vertical overflow, not an icon-spacing
problem: Fusion wraps a menu taller than the screen into multiple columns.
NeverD enables `SH_Menu_Scrollable` in the shared `WorkbenchStyle` on macOS,
Windows and Linux. Keep Qt's own scrolling, submenu and keyboard behavior;
check against `QScreen::availableGeometry()` in logical coordinates rather
than the monitor's physical pixel count.

For a disproportionately small tab-close glyph, compare an ordinary `QTabBar`
with the actual docking tab, including the style chain and the visible stroke
in logical pixels. KDDockWidgets installs a default `QProxyStyle` on its tab
bar to disable an unsafe removal animation. That proxy can resolve to the
native macOS style even when the application uses Fusion, shrinking the close
button's content rectangle. NeverD clears that local style so docking tabs
inherit `WorkbenchStyle`; its tab animation hint preserves the library's
safety workaround. Do not enlarge the shared SVG to compensate for an
unintended native metric. Check ordinary and docked tabs in both themes and
at native and fractional scale.

Check the close glyph's distance from the tab edge as well as its size. Qt's
styled tab text padding does not inset the separate close-button widget.
NeverD puts the close-button margin on the outer right side; a left margin
pushed the glyph against the edge even after its size was corrected. Inspect
the normal and hover states in a real docking tab, and measure the visible
stroke relative to `tabRect()`, not just the button's width. Keep the clickable
area intact when adjusting the painted inset.

## Platform font sizes and display scaling

Do not copy a Linux point size to every desktop. Qt uses logical DPI to turn
points into logical pixels: points × logical DPI / 72. Cocoa reports 72 DPI;
Linux and Windows commonly use a 96-DPI baseline. Thus a fixed 10 pt can mean
10 logical pixels on macOS and about 13.3 on another desktop. Retina's device
pixel ratio is a separate scale; do not multiply the font by it again.

Use `QFontDatabase::systemFont(GeneralFont)` for the reading size. NeverD keeps
its bundled Noto and JetBrains faces while inheriting that size for both UI
and code. Do not assume `systemFont(FixedFont)` gives a suitable editor size:
on the reproduced Cocoa kit it was 10 pt while GeneralFont was 13 pt, so using
FixedFont's size alone retained the small-code defect. Keep fractional point
sizes and pixel-sized preferences intact. Input fields, code zoom and reset
must share the same size policy; a reset must not restore the old literal.
Explicit user font choices still take precedence over defaults.

Separate monitor resolution, OS scaling, application zoom and font size when
comparing another application. In the reproduced case, a 3840×2160 display
already ran at a logical 1920×1080 with DPR 2. VS Code used zoom level 0,
a 12-logical-pixel editor font and an 18-pixel line height. That is diagnostic
evidence for that machine, not a universal size to hard-code. Normalize Retina
screenshots to logical coordinates before comparing text density.

`QTEST_MAIN` sets `AA_Use96Dpi` even when running on Cocoa. Native appearance
tests need an entry point without that forced DPI, otherwise they can render
larger fonts than the real application and miss this defect. NeverD's
AppearanceTests verifies platform sizes on Windows and macOS at native and
fractional scale; offscreen alone does not validate platform typography.

## Precision scrolling and zoom

A wheel notch is 120 angle units, but individual events may be smaller.
Trackpads also report `pixelDelta()`, including horizontal movement. Prefer
pixel deltas when present, retain partial rows and notches between events,
and avoid counting the pixel and angle versions of the same event twice.
An event sequence and its combined movement should scroll the same distance.
For text rows, keep integer remainders before division; per-event truncation
loses slow gestures. NeverD shares this behavior in `app/WheelInput`.

`ScrollBegin` and `ScrollEnd` can have no movement. Zero vertical delta must
not shrink a font, and horizontal scrolling with the zoom modifier must not
change its size. Accumulate precision zoom deltas too, rather than changing
one font point for every tiny event. Reset unconsumed input at gesture
boundaries and when switching between scrolling and zooming.

Qt maps portable Control shortcuts to Command on macOS. Keep shortcut storage
in `QKeySequence::PortableText`, show hints using `NativeText`, and verify the
actual event modifiers before adding platform-specific key swaps.

## Cocoa and Retina validation

- Give main-window geometry one owner. NeverD's application settings restore
  it at startup; automatic global/project desktop restoration only arranges
  panes. KDDockWidgets' default `LayoutSaver` temporarily unmaximizes, applies
  the saved normal frame, shows the window and maximizes it again. Repeating
  that during startup or after a file loads causes visible movement on Cocoa.
  Use `RestoreOption_RelativeToMainWindow` for automatic pane restoration and
  `RestoreOption_AbsoluteFloatingDockWindows` to preserve detached positions.
  This policy also applies on Windows/Linux. Explicitly loading a desktop can
  restore its full geometry. Test both normal and maximized main windows,
  including visibility before first show and move/resize/state events while
  loading a project; a final screenshot alone misses intermediate jumps.
- When an external link brings back a minimized workbench, clear only
  `Qt::WindowMinimized` from its window state before raising it. `showNormal()`
  also discards maximized/full-screen state, unexpectedly resizing the window.
  Keep this policy shared across macOS, Windows and Linux; verify ordinary
  and maximized windows both with and without minimization.
- Queue the application quit after the workbench accepts a close event.
  A synchronous `shutdownAccepted` → `QCoreApplication::quit` connection can
  reenter Cocoa's termination callback and crash Qt's thread-local cleanup.
  Verify native startup and shutdown with a loaded file, not only an empty
  window or an offscreen controller.
- Run focused tests with `QT_QPA_PLATFORM=cocoa` as well as offscreen. Use
  isolated `QSettings`, and exercise both themes and fractional scaling.
- A captured image stores physical pixels. Convert widget-local logical
  points to image coordinates with the capture's `devicePixelRatio()` before
  sampling colors; otherwise a Retina check can sample the neighboring pane
  or dialog background and misreport correct rendering as broken.
- Apply the workbench theme before testing widget geometry. macOS's native
  style can give `QWidgetItem` layout margins that differ from a widget's
  `sizeHint()`; a test using the native style does not establish how NeverD's
  Fusion-based style lays out the same control.
- Check `CGPreflightPostEventAccess()` before tests post native mouse events.
  `CGEventPost()` returning without an error does not establish delivery.
  Skip and report unavailable native-pointer coverage when permission is
  absent; do not classify those missed clicks as docking product defects.
- Observe AppKit completion when testing full-screen transitions. An eagerly
  updated Qt window-state flag is insufficient; NeverD's
  `tests/platform/NativeWindowStateMac.mm` provides that observation. First
  establish an active foreground test window. If macOS refuses activation,
  report native full-screen coverage as unavailable instead of interpreting
  the missing transition as a product regression.

## Development bundles

Keep the GUI, worker, MCP host and engine from the same source build. In an
application bundle the GUI finds its worker beside itself in `Contents/MacOS`.
Use NeverD's `package_macos.py` helper to deploy the matching Qt kit, repair
dependencies, audit the bundle and preserve engine-host entitlements while
signing. Check the packaged executable's startup and the bundled worker,
as well as the unbundled build. The normal package contains Cocoa's platform
plugin; offscreen tests run against the build tree, unless that plugin is
explicitly deployed for a separate test package.

Collect engine frameworks' transitive dependencies before `macdeployqt` changes
their original paths. It can rewrite a Python extension's reference to a
bundle-relative path without copying a keg-only library such as SQLite. Finish
dependency repair before signing: changing Mach-O load commands invalidates
signatures and can produce a macOS `CODESIGNING / Invalid Page` launch crash.
Assemble under a staging path and publish the final `.app` path only after
signing and a native startup/exit check succeed.

References: [Qt 6.7.3 menu-default change](https://code.qt.io/cgit/qt/qtreleasenotes.git/about/qt/6.7.3/release-note.md),
[QAction icon visibility](https://doc.qt.io/qt-6/qaction.html#iconVisibleInMenu-prop),
[QWheelEvent deltas and phases](https://doc.qt.io/qt-6/qwheelevent.html),
[Qt system fonts](https://doc.qt.io/qt-6/qfontdatabase.html#systemFont),
[logical DPI and device-pixel ratio](https://doc.qt.io/qt-6/qscreen.html#logicalDotsPerInch-prop).
