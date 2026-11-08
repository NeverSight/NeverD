//===- PageDelta.h - What changed between two pages of a view -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The operations that make a page of a view from the page a client keeps:
/// fields set and removed, lines of a text replaced, and runs of a list kept,
/// shifted, replaced, dropped or added.  An edit that changes a few lines of a
/// view, and moves the byte offsets of what follows them, changes a page by a
/// few operations, which a client applies instead of reading the page whole.
/// PROTOCOL.md "page_delta" is the wire form.
///
//===----------------------------------------------------------------------===//

#pragma once
#include "Protocol.h"

#include <optional>

namespace neverd::worker {

/// The operations that make \p after from \p before, both page objects, or
/// nothing where they are not objects.
std::optional<Json> pageDelta(const Json &before, const Json &after);

/// \p page with \p delta applied, or nothing where an operation does not fit
/// it.
std::optional<Json> applyPageDelta(Json page, const Json &delta);

} // namespace neverd::worker
