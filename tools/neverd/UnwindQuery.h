//===- UnwindQuery.h - Exact-PC CLI query vocabulary ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared vocabulary for registering and executing the unwind-frame query.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_UNWINDQUERY_H
#define NEVERD_TOOLS_NEVERD_UNWINDQUERY_H

namespace neverd::cli::unwind_query {

#define NEVERD_UNWIND_CLI_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "UnwindQuery.def"

} // namespace neverd::cli::unwind_query

#endif // NEVERD_TOOLS_NEVERD_UNWINDQUERY_H
