//===- ProjectFileWatcher.h - Project sidecar watching -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Project file notifications and generation-bound sidecar reads.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_GUI_APP_PROJECTFILEWATCHER_H
#define NEVERD_TOOLS_GUI_APP_PROJECTFILEWATCHER_H

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QString>

namespace neverd::gui {

/// Lives on the project's I/O thread. Native watch registration can enumerate
/// directories and wait for an OS permission prompt, just like a file read.
class ProjectFileWatcher final : public QObject {
  Q_OBJECT
public:
  using Files = QHash<QString, QByteArray>;

  ProjectFileWatcher();
  void read(const QString &input, quint64 generation);
  void clear();

signals:
  void changed(quint64 generation);
  void filesReady(quint64 generation, const Files &files);

private:
  QFileSystemWatcher Watcher;
  QString Input;
  quint64 Generation = 0;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_GUI_APP_PROJECTFILEWATCHER_H
