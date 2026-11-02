//===- AArch64SVECPA.h - Scalable checked pointer contracts ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Representation-independent operation identity and operand roles. Runtime
/// pointer checks remain machine operations, not ordinary vector arithmetic.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVECPA_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVECPA_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <optional>

namespace neverd::aarch64_sve {

#define NEVERD_A64_SVE_CPA_VALUE(Type, Name, Value)                            \
  inline constexpr Type Name = Value;
#define NEVERD_A64_SVE_CPA_TEXT(Name, Value)                                   \
  inline constexpr char Name[] = Value;
#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.def"

enum class CPAForm {
#define NEVERD_A64_SVE_CPA_FORM(Name, ...) Name,
#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.def"
};

struct CPARoles {
  unsigned Count;
  unsigned Vectors;
  unsigned Predicates;
  unsigned RawCount;
};

constexpr CPARoles cpaRoles(CPAForm Form) {
  switch (Form) {
#define NEVERD_A64_SVE_CPA_FORM(Name, Count, Vectors, Predicates, RawCount)    \
  case CPAForm::Name:                                                          \
    return {Count, Vectors, Predicates, RawCount};
#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.def"
  }
  return {};
}

struct CPAOperation {
  Intrinsic Id;
  CPAForm Form;
  uint32_t Mask;
  uint32_t Encoding;
  const char *MachineAsm;
  const char *Constraints;
};

inline constexpr CPAOperation kCPAOperations[] = {
#define NEVERD_A64_SVE_CPA_OPERATION(Id, Insn, Name, Form, Mask, Encoding,     \
                                     Asm, Constraints)                         \
  {Intrinsic::Id, CPAForm::Form, Mask, Encoding, Asm, Constraints},
#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.def"
};

inline std::optional<CPAOperation> cpaOperation(Intrinsic Id) {
  for (const auto &Operation : kCPAOperations)
    if (Operation.Id == Id)
      return Operation;
  return std::nullopt;
}

} // namespace neverd::aarch64_sve

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVECPA_H
