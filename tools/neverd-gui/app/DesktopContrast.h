//===- DesktopContrast.h - The desktop's high contrast setting -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Whether the desktop asks applications for high contrast colors.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_DESKTOPCONTRAST_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_DESKTOPCONTRAST_H

namespace neverd::gui::platform {

/// The contrast the desktop asks for, and whether its high contrast colors
/// are dark or light.
enum class DesktopContrast { Normal, HighDark, HighLight };

/// Windows' High Contrast themes, dark or light by their window color, and
/// macOS's Increase Contrast, dark or light by the application's appearance.
/// Visual Studio Code switches to its high contrast themes for both; other
/// desktops report Normal, where a person chooses a high contrast theme.
DesktopContrast desktopContrast();

} // namespace neverd::gui::platform

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_DESKTOPCONTRAST_H
