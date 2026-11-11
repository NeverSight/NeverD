//===- DesktopContrastWindows.cpp - Windows high contrast -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read whether a Windows High Contrast theme is on, and how bright it is.
//===----------------------------------------------------------------------===//

#include "DesktopContrast.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace neverd::gui::platform {

DesktopContrast desktopContrast() {
  HIGHCONTRASTW contrast{};
  contrast.cbSize = sizeof(contrast);
  if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, contrast.cbSize, &contrast,
                             0) ||
      !(contrast.dwFlags & HCF_HIGHCONTRASTON))
    return DesktopContrast::Normal;
  // A High Contrast theme's window color tells its dark themes from its light.
  const COLORREF window = GetSysColor(COLOR_WINDOW);
  const double luminance = 0.2126 * GetRValue(window) +
                           0.7152 * GetGValue(window) +
                           0.0722 * GetBValue(window);
  return luminance < 128 ? DesktopContrast::HighDark
                         : DesktopContrast::HighLight;
}

} // namespace neverd::gui::platform
