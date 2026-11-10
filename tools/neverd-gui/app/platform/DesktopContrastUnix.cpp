//===- DesktopContrastUnix.cpp - Other desktops' contrast -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Desktops without a contrast setting the workbench reads.
//===----------------------------------------------------------------------===//

#include "DesktopContrast.h"

namespace neverd::gui::platform {

DesktopContrast desktopContrast() { return DesktopContrast::Normal; }

} // namespace neverd::gui::platform
