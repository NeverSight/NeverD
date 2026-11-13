//===- Keyring.h - Native user credential storage ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Blocking OS keyring operations, called from the credential I/O thread.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_PLATFORM_KEYRING_H
#define NEVERD_TOOLS_AGENT_PLATFORM_KEYRING_H

#include <QByteArray>
#include <QString>

namespace neverd::agent::keyring {
bool store(const QString &id, const QByteArray &secret, QString &error);
bool read(const QString &id, QByteArray &secret, QString &error);
bool remove(const QString &id);
} // namespace neverd::agent::keyring

#endif // NEVERD_TOOLS_AGENT_PLATFORM_KEYRING_H
