//===- Credentials.h - User-local model credentials -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separate secret storage from exportable configuration and project data.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_MODELS_CREDENTIALS_H
#define NEVERD_TOOLS_AGENT_MODELS_CREDENTIALS_H

#include <QString>

namespace neverd::agent {
class Credentials {
public:
  explicit Credentials(QString directory);
  bool store(const QString &key, QString &id, QString &error) const;
  bool read(const QString &id, QString &key, QString &error) const;
  bool remove(const QString &id) const;
  static bool validateKey(const QString &key, QString &error);

private:
  QString path(const QString &id) const;
  QString Directory;
};
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_MODELS_CREDENTIALS_H
