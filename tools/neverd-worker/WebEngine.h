//===- WebEngine.h - Offline analysis worker adapter -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline analysis worker adapter.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "Protocol.h"
#include "common/web/Backend.h"

namespace neverd::worker {
using WebEngine = web_client::Backend;
} // namespace neverd::worker
