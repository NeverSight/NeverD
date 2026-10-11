//===- SourceValues.h - Finite primitive value evidence ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finite primitive value evidence.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Source.h"

#include <memory>

namespace neverd::web {

inline constexpr std::string_view JavaScriptValueProfile =
    "javascript-primitive-values-v1";
inline constexpr uint64_t MaxJavaScriptValueSteps = 4000000;
inline constexpr uint64_t MaxJavaScriptValueStringUnits = 65536;
inline constexpr uint64_t MaxJavaScriptValueAllocatedUnits = 2097152;
inline constexpr unsigned MaxJavaScriptBigIntBits = 1024;

enum class PrimitiveKind { Undefined, Null, Boolean, Number, String, BigInt };

// Private, immutable values, never ordinary output metadata. Number stores
// binary64 bits (including -0); BigInt stores a canonical signed decimal.
struct SourcePrimitive {
  PrimitiveKind Kind = PrimitiveKind::Undefined;
  bool Boolean = false;
  uint64_t NumberBits = 0;
  std::u16string String;
  std::string BigInt;
};

struct SourceValue {
  // constant, unknown, would_throw, unsupported, budget_exceeded,
  // or not_expression. Constant describes evaluation under this profile,
  // not reachability or permission to replace/delete source text.
  std::string Status = "not_expression";
  std::string Reason;
  std::shared_ptr<const SourcePrimitive> Value;
};

struct SourceValueAnalysis {
  std::string ID;
  std::string SourceID;
  std::string Status;
  // Indexed by the source model's syntax node occurrence.
  std::vector<SourceValue> Nodes;
  std::vector<SourceDiagnostic> Diagnostics;
  uint64_t Steps = 0;
  uint64_t AllocatedStringUnits = 0;
};

bool sourcePrimitiveTruthy(const SourcePrimitive &Value);
std::string_view sourcePrimitiveKindName(PrimitiveKind Kind);

// Literal primitives and an explicit operation whitelist only. Never invokes
// target code, resolves an identifier by spelling, or accesses an object.
SourceValueAnalysis analyzeSourceValues(const SourceAnalysis &Source);

} // namespace neverd::web
