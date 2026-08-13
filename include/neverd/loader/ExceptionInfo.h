//===- ExceptionInfo.h - Normalized exception metadata --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the format-independent representation used to carry table-based
/// unwind and language exception metadata from loaders through NeverD's IR and
/// rewrite pipelines.  Raw file offsets never escape the loader: consumers see
/// checked half-open VA ranges, normalized targets, parse provenance, and an
/// explicit completeness state.
///
/// This is an umbrella header.  Each part of the representation lives in a
/// header of its own, and this one names them all so that a consumer keeps one
/// include for the whole vocabulary.  Include a part directly when only that
/// part is needed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_EXCEPTIONINFO_H
#define NEVERD_LOADER_EXCEPTIONINFO_H

#include "neverd/loader/ExceptionCommon.h"
#include "neverd/loader/ExceptionEdge.h"
#include "neverd/loader/ExceptionEncoding.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/loader/ExceptionModel.h"
#include "neverd/loader/ExceptionPersonality.h"
#include "neverd/loader/ExceptionTable.h"
#include "neverd/loader/ExceptionUnwindOp.h"
#include "neverd/loader/ExceptionWindowsEH.h"
#include "neverd/loader/LanguageEH.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#endif // NEVERD_LOADER_EXCEPTIONINFO_H
