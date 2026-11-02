#pragma once

#include "Session.h"

namespace neverd::gui {
// Same-build test seam: delivers a worker message as the session's worker
// connection does. Tests only; the workbench never includes this header.
struct SessionTestAccess {
  static void receive(Session &S, const QJsonObject &Message) {
    S.receive(Message);
  }
  static QString nextBackgroundSequence(const Session &S) {
    return QString::number(S.BackgroundSequence.value_or(0) + 1);
  }
};
} // namespace neverd::gui
