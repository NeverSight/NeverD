#include "NativeLayoutSaver.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

NativeLayoutSaver::NativeLayoutSaver(QObject *parent)
    : KDDockWidgets::LayoutSaverInstantiator(parent) {}

bool NativeLayoutSaver::saveToFile(const QString &jsonFilename) {
  const auto layout = serializeLayout();
  if (layout.isEmpty())
    return false;
  {
    QFile previous(jsonFilename);
    if (previous.open(QIODevice::ReadOnly) &&
        previous.size() == layout.size() && previous.readAll() == layout)
      return true;
  }
  QSaveFile output(jsonFilename);
  // Keep QSaveFile's atomic behavior even when a direct write would succeed.
  output.setDirectWriteFallback(false);
  if (!output.open(QIODevice::WriteOnly) ||
      output.write(layout) != layout.size() || !output.commit()) {
    qWarning("Could not save dock layout %s: %s", qPrintable(jsonFilename),
             qPrintable(output.errorString()));
    return false;
  }
  return true;
}

bool NativeLayoutSaver::restoreFromFile(const QString &jsonFilename) {
  if (!QFileInfo::exists(jsonFilename))
    return false;
  return KDDockWidgets::LayoutSaverInstantiator::restoreFromFile(jsonFilename);
}
