//===- Reader.h - Private nexe reader boundaries ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared accounting and index admission for the pinned nexe envelope.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Nexe.h"

namespace neverd::web::nexe_detail {
void step(NexeExtraction &E, uint64_t Count = 1);
void readIndex(const Artifact &Input, std::string_view Bootstrap,
               NexeExtraction &E);
} // namespace neverd::web::nexe_detail
