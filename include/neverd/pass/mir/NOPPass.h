//===- NOPPass.h - NOP insertion pass ------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A trivial MIRPass that appends a NOP instruction to each function's
/// machine code buffer, useful for testing the pass infrastructure.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_MIR_NOPPASS_H
#define NEVERD_PASS_MIR_NOPPASS_H

#include "neverd/pass/mir/MIRPass.h"

namespace neverd {

class NopPass : public MIRPass {
public:
  bool run(MIRPassContext &Ctx) override;
  const char *name() const override { return "nop"; }
};

} // namespace neverd

#endif // NEVERD_PASS_MIR_NOPPASS_H
