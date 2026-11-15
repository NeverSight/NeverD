//===- ProjectFileWatcher.cpp - Project sidecar watching ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Register project file watches and read sidecars on the I/O thread.
//===----------------------------------------------------------------------===//

#include "ProjectFileWatcher.h"

#include "ProjectDatabase.h"

#include <QFile>
#include <QFileInfo>

namespace neverd::gui {

ProjectFileWatcher::ProjectFileWatcher() : Watcher(this) {
  connect(&Watcher, &QFileSystemWatcher::directoryChanged, this,
          [this] { emit changed(Generation); });
  connect(&Watcher, &QFileSystemWatcher::fileChanged, this,
          [this] { emit changed(Generation); });
}

void ProjectFileWatcher::clear() {
  if (!Watcher.directories().isEmpty())
    Watcher.removePaths(Watcher.directories());
  if (!Watcher.files().isEmpty())
    Watcher.removePaths(Watcher.files());
  Input.clear();
}

void ProjectFileWatcher::read(const QString &input, quint64 generation) {
  if (input != Input || generation != Generation) {
    clear();
    Input = input;
    Generation = generation;
  }
  // The parent sees the project folder appear after a pull or first edit;
  // the project folder sees atomic file replacements. Avoid re-registering
  // an existing directory, which flushes the native event stream on macOS.
  const auto directories = Watcher.directories();
  const QString parent = QFileInfo(input).absolutePath();
  const QString folder = ProjectDatabase::projectFolder(input);
  QStringList added;
  if (!directories.contains(parent))
    added.append(parent);
  if (!directories.contains(folder) && QFileInfo(folder).isDir())
    added.append(folder);
  if (!added.isEmpty())
    Watcher.addPaths(added);

  const auto current = Watcher.files();
  Files files;
  added.clear();
  for (const auto &suffix : ProjectDatabase::sidecarSuffixes()) {
    const QString path = ProjectDatabase::projectFilePath(input, suffix);
    QFile file(path);
    files.insert(suffix, file.open(QIODevice::ReadOnly) ? file.readAll()
                                                        : QByteArray());
    if (file.exists() && !current.contains(path))
      added.append(path);
  }
  if (!added.isEmpty())
    Watcher.addPaths(added);
  emit filesReady(generation, files);
}

} // namespace neverd::gui
