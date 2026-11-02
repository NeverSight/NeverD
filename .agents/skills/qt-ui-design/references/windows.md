# Windows desktop UI notes

Read this for Windows custom frames, dialog attachment, transparency, and
shadows. The implementation details below describe NeverD's app-drawn frame;
other products may keep the system frame and its DWM shadow.

## Configure transparency before updating the native flags

A dialog can already have an HWND before `WindowFrame::attach`: an early
`winId()` call or Qt's dialog polish can create it. An unshown dialog does not
necessarily have a fresh native window.

For NeverD's translucent frame, preserve this order in `WindowFrame::attach`:

1. Record the widget's `Qt::FramelessWindowHint` and
   `Qt::NoDropShadowWindowHint` with `overrideWindowFlags`, which leaves the
   existing native flags untouched at this step.
2. Set `Qt::WA_TranslucentBackground` on the widget.
3. If a `QWindow` already exists, take its `requestedFormat()`, set
   `alphaBufferSize` to 8, and apply it with `setFormat()`.
4. Only then call that handle's `setFlags(window.windowFlags())`. For a window
   without a handle, create it after the widget's flags and transparency
   attribute have been configured.

Windows selects the per-pixel transparency path when the native flags change.
Applying them before the alpha format can leave an existing HWND opaque, so
the whole shadow margin appears as a solid black border even though the widget
itself paints transparent pixels. Changing QSS or the shadow color does not
correct that native presentation path. Keep this ordering at the shared frame
owner so ordinary, stock, and Quick Start dialogs receive the same fix.

NeverD draws the edge and shadow itself. Its Windows platform integration
disables DWM non-client rendering and clears the extended frame margins;
avoid adding a second system shadow. Preserve native window-management
behavior while changing the appearance.

## Verify pixels composed by the Windows desktop

`QWidget::grab()` proves what Qt painted, but cannot establish that Windows
composed those pixels with transparency. The regression must also observe the
displayed window:

- Run with `QT_QPA_PLATFORM=windows` on an accessible Windows desktop. Check
  the actual platform plugin; a Windows executable using `offscreen` still
  cannot validate desktop composition. Report skipped native checks as skipped.
- Place the dialog over a known solid backdrop. Compose a painted shadow
  sample over that color to obtain the expected pixel, then compare it with a
  desktop capture from `QScreen::grabWindow(0, ...)`. This catches an opaque
  native window without hard-coding a particular shadow color.
- Wait for native exposure before checking placement and sampling the shadow.
  A hidden native window can still report an old screen position. Confirm the
  sample lies over the backdrop and wait for the expected composed pixel.
- Cover ordinary and Quick Start dialogs, each with and without early
  `winId()` creation, through both `show()` and modal `exec()`.
- Check at 100% and a fractional scale such as 150%, with light and dark
  dialog appearance. Retain focused edge/corner resize, maximize, full-screen,
  and restore checks when changing the frame or native flags.

Use the shipped Qt build and Release configuration. Build the affected targets
through the repository's supported entry point:

```powershell
$guiBuildDir = 'build-release'
python scripts/build.py $guiBuildDir --config Release --target neverd-gui neverd-gui-appearance-tests neverd-gui-unit-tests --parallel 4
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_SCALE_FACTOR = '1'
& "$guiBuildDir/bin/neverd-gui-appearance-tests.exe" windowShadowCompositesWithDesktop dialogsUseOwnedChrome
$env:QT_SCALE_FACTOR = '1.5'
& "$guiBuildDir/bin/neverd-gui-appearance-tests.exe" windowShadowCompositesWithDesktop dialogsUseOwnedChrome
$env:QT_SCALE_FACTOR = '1'
& "$guiBuildDir/bin/neverd-gui-unit-tests.exe" nativeFrameKeepsTheTitleBarOnScreen
ctest --test-dir $guiBuildDir --build-config Release -R '^NeverDGuiSmoke$' --output-on-failure
```

Set `NEVERD_APPEARANCE_CAPTURES` to an output directory to retain the composited
desktop captures. Offscreen runs remain useful for widget and interaction
checks; the desktop-shadow rows intentionally skip there. If a Windows desktop
is unavailable, state that native composition has not been verified.

## Owning code and evidence

- [WindowFrame.cpp](../../../../tools/neverd-gui/app/WindowFrame.cpp): shared
  frame attachment and alpha-format setup.
- [WindowFrameWindows.cpp](../../../../tools/neverd-gui/app/platform/WindowFrameWindows.cpp):
  Windows native frame integration.
- [AppearanceTests.cpp at the merged revision](https://github.com/NeverSight/NeverD/blob/0578bf048ac71730cdf3f44026478f9064fc5420/tools/neverd-gui/tests/AppearanceTests.cpp):
  `windowShadowCompositesWithDesktop` and `dialogsUseOwnedChrome`.
- [GUI testing guide](../../../../docs/testing.md#gui-function-lists-and-navigation):
  native capture requirements and focused GUI checks.
- [NeverD PR #1805](https://github.com/NeverSight/NeverD/pull/1805), reviewed
  head `92b382cf8cbc9ef0eb17581f86702a69bf3c5e6b`: the demonstrated existing-HWND
  failure, initialization-order fix, and native Windows validation.
