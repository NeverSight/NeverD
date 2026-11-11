//===- DesktopContrastMac.mm - macOS increased contrast -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read whether macOS's Increase Contrast is on, and the appearance it shows.
//===----------------------------------------------------------------------===//

#include "DesktopContrast.h"

#import <AppKit/AppKit.h>

namespace neverd::gui::platform {

DesktopContrast desktopContrast() {
  if (!NSWorkspace.sharedWorkspace.accessibilityDisplayShouldIncreaseContrast)
    return DesktopContrast::Normal;
  // Called once the application exists, so NSApp is set.
  NSAppearanceName name =
      [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:@[
        NSAppearanceNameAqua, NSAppearanceNameDarkAqua
      ]];
  return [name isEqualToString:NSAppearanceNameDarkAqua]
             ? DesktopContrast::HighDark
             : DesktopContrast::HighLight;
}

} // namespace neverd::gui::platform
