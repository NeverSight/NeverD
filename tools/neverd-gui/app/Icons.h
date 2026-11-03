#pragma once

#include <QIcon>
#include <QString>

class QAbstractFileIconProvider;

namespace neverd::gui {

/// Original workbench icon \p name (resources/icons/<name>.svg), rendered
/// from SVG at every requested size and device pixel ratio without relying
/// on an icon-engine plugin.  Unknown names yield a null icon.
QIcon icon(const QString &name);

/// Local PNG/ICO or SVG icon. SVG uses the same DPI-aware renderer as built-in
/// icons; unknown, oversized or malformed images yield a null icon.
QIcon iconFromFile(const QString &path);

/// The workbench's icons for the folders, files, drives and computer a file
/// dialog lists, in place of the platform icon theme's.
QAbstractFileIconProvider *fileIcons();

} // namespace neverd::gui
