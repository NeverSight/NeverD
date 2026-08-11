//===- Semantics.h - Normalized Solana SBF semantics ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the version-normalized instruction traits shared by analysis,
/// interpretation, and every source/LLVM backend.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SEMANTICS_H
#define NEVERD_SBF_SEMANTICS_H

#include "neverd/sbf/Opcodes.h"

#include <cstdint>

namespace neverd::sbf {

enum class ResultExtension : uint8_t { None, Zero32, Sign32 };
enum class ImmediateExtension : uint8_t { Sign32, Zero32, Full64 };

enum class OperandSourceKind : uint8_t {
  None,
  Immediate,
  SourceRegister,
  VersionedCallRegister,
};

enum class FaultPolicy : uint8_t {
  None = 0,
  DivideByZero = 1u << 0,
  DivideOverflow = 1u << 1,
  MemoryAccess = 1u << 2,
  Call = 1u << 3,
};

constexpr FaultPolicy operator|(FaultPolicy Left, FaultPolicy Right) {
  return static_cast<FaultPolicy>(static_cast<uint8_t>(Left) |
                                  static_cast<uint8_t>(Right));
}

constexpr bool hasFaultPolicy(FaultPolicy Set, FaultPolicy Policy) {
  return (static_cast<uint8_t>(Set) & static_cast<uint8_t>(Policy)) != 0;
}

enum class TerminatorKind : uint8_t {
  None,
  Jump,
  ConditionalJump,
  Call,
  Return,
};

struct SemanticTraits {
  ImmediateExtension Immediate = ImmediateExtension::Sign32;
  ResultExtension Result = ResultExtension::None;
  OperandSourceKind Source = OperandSourceKind::None;
  FaultPolicy Faults = FaultPolicy::None;
  TerminatorKind Terminator = TerminatorKind::None;
  uint8_t MemoryWidth = 0;
  bool WritesDestination = false;
  bool SwapOperands = false;
};

SemanticTraits semanticTraits(const OpcodeInfo &Info, Version V);
uint64_t normalizeImmediate(uint64_t Immediate, ImmediateExtension Extension);
uint64_t extendALU32Result(uint32_t Value, ResultExtension Extension);
int64_t callxRegisterIndex(Version V, uint8_t Dst, uint8_t Src,
                           int32_t Immediate);

} // namespace neverd::sbf

#endif // NEVERD_SBF_SEMANTICS_H
