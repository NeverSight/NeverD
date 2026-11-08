//===- PageDelta.h - A kept page with what changed applied ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The client's half of page_delta (tools/neverd-worker/PROTOCOL.md): a worker
/// that still has the document a kept page came from sends the operations that
/// make the page now, and the client applies them to the page it keeps.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_PAGEDELTA_H
#define NEVERD_GUI_PAGEDELTA_H

#include <QJsonArray>
#include <QJsonObject>
#include <optional>

namespace page_delta {

/// \p page with \p delta applied, or nothing where an operation does not fit
/// it.
std::optional<QJsonObject> apply(QJsonObject page, const QJsonArray &delta);

} // namespace page_delta

#endif // NEVERD_GUI_PAGEDELTA_H
