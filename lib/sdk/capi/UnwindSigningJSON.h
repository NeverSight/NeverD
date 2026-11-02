//===- UnwindSigningJSON.h - Public signing query serialization -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Present the loader's two independently scoped results without fallback.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_UNWINDSIGNINGJSON_H
#define NEVERD_SDK_CAPI_UNWINDSIGNINGJSON_H

#include "JSONText.h"

#include "neverd/loader/ReturnAddressSigning.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <optional>
#include <string>
#include <utility>

namespace neverd::sdk {
namespace unwind_json {

#define NEVERD_RA_JSON_TEXT(Name, Text) inline constexpr char Name[] = Text;
#define NEVERD_RA_JSON_STATUS(Value, Name, Text)                               \
  inline constexpr char Name[] = Text;
#define NEVERD_RA_JSON_KIND(Value, Name, Text)                                 \
  inline constexpr char Name[] = Text;
#include "UnwindSigningJSON.def"

inline const char *statusName(dwarf_eh::AArch64RAQueryStatus Status) {
  switch (Status) {
#define NEVERD_RA_JSON_STATUS(Value, Name, Text)                               \
  case dwarf_eh::AArch64RAQueryStatus::Value:                                  \
    return Name;
#include "UnwindSigningJSON.def"
  }
  return kUnknown;
}

inline const char *kindName(dwarf_eh::AArch64RASigningKind Kind) {
  switch (Kind) {
#define NEVERD_RA_JSON_KIND(Value, Name, Text)                                 \
  case dwarf_eh::AArch64RASigningKind::Value:                                  \
    return Name;
#include "UnwindSigningJSON.def"
  }
  return kUnknown;
}

inline std::string address(va_t Address) {
  return std::string(kAddressPrefix) + llvm::utohexstr(Address);
}

inline llvm::json::Value
sourceResult(const std::optional<dwarf_eh::AArch64RAQueryResult> &Result) {
  if (!Result)
    return nullptr;
  llvm::json::Object Object{{kStatus, statusName(Result->Status)}};
  if (Result->Diagnostic)
    Object[kDiagnostic] = jsonSafeText(Result->Diagnostic);
  if (Result->Status == dwarf_eh::AArch64RAQueryStatus::Known &&
      Result->State) {
    const auto &State = *Result->State;
    Object[kKind] = kindName(State.Kind);
    if (State.Kind != dwarf_eh::AArch64RASigningKind::Unsigned)
      Object[kKey] = State.Key == dwarf_eh::AArch64RAKey::A ? kKeyA : kKeyB;
    if (State.Kind == dwarf_eh::AArch64RASigningKind::SPAndPC) {
      Object[kRecipe] = State.Recipe == dwarf_eh::AArch64RARecipe::HintPACM
                            ? kHintPACM
                            : kNonHintPAuthLR;
      if (State.SigningPC)
        Object[kSigningPC] = address(*State.SigningPC);
    }
  }
  return llvm::json::Value(std::move(Object));
}

} // namespace unwind_json

inline llvm::json::Object
unwindSigningJSON(const AArch64UnwindSigningQuery &Query) {
  return llvm::json::Object{
      {unwind_json::kQueryPC, unwind_json::address(Query.QueryPC)},
      {unwind_json::kDwarfStateAtPC,
       unwind_json::sourceResult(Query.DwarfStateAtPC)},
      {unwind_json::kCompactRecipe,
       unwind_json::sourceResult(Query.CompactRecipe)}};
}

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_UNWINDSIGNINGJSON_H
