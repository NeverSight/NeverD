//===- UnwindQuery.h - Exact-address unwind transport ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Optional public ABI lookup shared by exact-address queries and listing
/// frame markers. No return-address state is inferred in this adapter.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WORKER_UNWINDQUERY_H
#define NEVERD_WORKER_UNWINDQUERY_H

#include "EngineSymbols.h"

#include <string_view>

namespace neverd::worker::unwind_query {

#define NEVERD_UNWIND_QUERY_TEXT(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#define NEVERD_UNWIND_QUERY_VALUE(Type, Name, Value)                           \
  inline constexpr Type Name = Value;
#define NEVERD_UNWIND_QUERY_ADDRESS(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "UnwindQuery.def"

using Function = const char *(*)(neverd_session_t, neverd_va_t);

inline Function function() {
  static const auto Query = engineSymbol<Function>(kSymbol);
  return Query;
}

inline bool isAddressField(std::string_view Key) {
#define NEVERD_UNWIND_QUERY_ADDRESS(Name, Text)                                \
  if (Key == Name)                                                             \
    return true;
#include "UnwindQuery.def"
  return false;
}

} // namespace neverd::worker::unwind_query

#endif // NEVERD_WORKER_UNWINDQUERY_H
