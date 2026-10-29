//===- RecoveryDigest.h - Evidence encoding ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Canonical recovery evidence encoding.

#ifndef NEVERD_ANALYSIS_CORE_RECOVERYDIGEST_H
#define NEVERD_ANALYSIS_CORE_RECOVERYDIGEST_H

#include "neverd/analysis/InterpreterSpecialization.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/SHA256.h"

namespace neverd::analysis {
// Versioned, length-prefixed bytes and little-endian numbers. Never hash C++
// object representation, pointer values, padding or host byte order.
class RecoveryDigest {
  llvm::SHA256 Hash;

public:
  explicit RecoveryDigest(llvm::StringRef Version) { text(Version); }
  void number(uint64_t Value) {
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Value);
    Hash.update(llvm::ArrayRef<uint8_t>(Bytes));
  }
  void bytes(llvm::ArrayRef<uint8_t> Bytes) {
    number(Bytes.size());
    Hash.update(Bytes);
  }
  void text(llvm::StringRef Text) {
    number(Text.size());
    Hash.update(Text);
  }
  void variable(const NdVar &V) {
    number(static_cast<unsigned>(V.Space));
    number(V.Offset);
    number(V.Size);
    number(static_cast<unsigned>(V.Provenance));
    number(V.AddressOwnerVA);
  }
  void boundary(const LowInstructionBoundary &B) {
    number(B.Address);
    number(B.Size);
    number(B.FirstOp);
    number(B.OpCount);
    number(static_cast<unsigned>(B.Mode));
    number(static_cast<unsigned>(B.Control));
    number(static_cast<unsigned>(B.ControlFlags));
    number(static_cast<unsigned>(B.TargetMode));
    number(B.Immediate.has_value());
    if (B.Immediate)
      number(*B.Immediate);
  }
  /// Binds the first \p InputSlots stored input slots, operands or not.
  void operation(const LowOp &Op, size_t InputSlots) {
    number(static_cast<unsigned>(Op.Opcode));
    number(Op.Addr);
    number(Op.Seq);
    variable(Op.Output);
    number(Op.NumInputs);
    for (size_t I = 0; I != InputSlots; ++I)
      variable(Op.Inputs[I]);
    number(static_cast<unsigned>(Op.MemoryOrdering));
    number(static_cast<unsigned>(Op.MemoryAddressSpace));
  }
  template <typename Range> void numbers(const Range &Values) {
    number(Values.size());
    for (const auto V : Values)
      number(V);
  }
  std::string finish() { return llvm::toHex(Hash.final()); }
};

/// Shared specialization policy, after the architecture's execution profile.
/// The architecture owns the version and placement within its options schema.
void appendRawEnvironmentPolicy(RecoveryDigest &Hash,
                                const SpecializationOptions &Options);
} // namespace neverd::analysis
#endif
