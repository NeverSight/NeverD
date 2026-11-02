//===- LinkDispatch.h - A link clicked anywhere ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// What `neverd-gui --link URL` does, as IDA's hcli does for ida:// links:
/// the running workbench that has the link's program takes the link
/// through its InstanceServer, and only when none has does this process
/// open the program, a file the user opened before by the link's name, and
/// then the link.  It also lists the running workbenches, and makes the
/// workbench the system's handler of neverd:// links.
//===----------------------------------------------------------------------===//

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <optional>

namespace neverd::gui {

struct DeepLink;

/// The running workbench, of those whose get_info answers are
/// \p instances, that takes \p link: one whose input the link's SHA-256
/// prefix names, else one whose file the link's input names, else, for a
/// link that names no program, the only one with a program open.
struct InstanceChoice {
  /// Its index, or -1 for none.
  int index = -1;
  /// Several have programs open and the link names none of them.
  bool ambiguous = false;
};
InstanceChoice chooseInstance(const QList<QJsonObject> &instances,
                              const DeepLink &link);

struct Dispatched {
  /// A running workbench took the link, or several could have.
  bool sent = false;
  /// It opened the link.
  bool opened = false;
  /// What happened, for the user.
  QString message;
};
/// Sends \p text, which parses as \p link, to the running workbench that
/// takes it.
Dispatched dispatchLink(const QString &text, const DeepLink &link);

/// The running workbenches, a line each: the process, its program and
/// whether its analysis is done.
QString describeInstances();

/// A recently opened input or database. A hash outranks names; a database
/// names its stored input, whose bytes unpack verifies before navigation.
std::optional<QString> recentFileFor(const DeepLink &link);

/// Makes this executable the system's handler of neverd:// links, or stops
/// it being that; false when it could not, \p message saying what happened.
bool registerLinkHandler(bool install, QString &message);

} // namespace neverd::gui
