#pragma once

#include <LayoutSaverInstantiator.h>

// Preserve KDDockWidgets' layout format and restoration, with atomic writes.
class NativeLayoutSaver : public KDDockWidgets::LayoutSaverInstantiator {
  Q_OBJECT
public:
  explicit NativeLayoutSaver(QObject *parent = nullptr);
  Q_INVOKABLE bool saveToFile(const QString &jsonFilename);
  Q_INVOKABLE bool restoreFromFile(const QString &jsonFilename);
};
