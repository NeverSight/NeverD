//===- SymConcrete.h - Exact concrete execution through SymExec -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the concrete half of a concolic LowIR trace.  It runs a second
/// SymExec whose entry registers are constants.  Constant folding therefore
/// supplies concrete values without copying NdOp or width semantics into a
/// separate interpreter, while any missing input remains symbolic and fails
/// closed.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMCONCRETE_H
#define NEVERD_SYMBOLIC_SYMCONCRETE_H

#include "neverd/symbolic/SymExplore.h"

#include <memory>

namespace neverd::symbolic {

/// A loader-neutral, width-exact concrete shadow backed by SymExec itself.
///
/// Register seeds are byte ranges of at most 64 bits.  They are written only
/// into this shadow's private state; the primary symbolic state remains free so
/// its expressions retain structured entry-input origins.  Missing or
/// overlapping seed bytes, initial memory, fresh values, unmodelled operations,
/// and conservative havoc all make the shadow decline the trace.
class SymExecConcreteShadow final : public ConcreteShadow {
public:
  SymExecConcreteShadow();
  ~SymExecConcreteShadow() override;

  SymExecConcreteShadow(const SymExecConcreteShadow &) = delete;
  SymExecConcreteShadow &operator=(const SymExecConcreteShadow &) = delete;

  bool reset(llvm::endianness ByteOrder) override;
  bool setRegister(uint64_t Offset, uint16_t Bytes, uint64_t Value) override;
  bool step(const LowOp &Op) override;
  std::optional<uint64_t> value(const NdVar &V) override;

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMCONCRETE_H
