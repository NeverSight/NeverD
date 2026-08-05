//===- NOPPass.cpp - NOP insertion pass ----------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Test MIR pass that NOP-fills function bodies.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/mir/NOPPass.h"

#include "neverd/Support/TargetCodegenInfo.h"

#define DEBUG_TYPE "neverd-nop-pass"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {

bool NopPass::run(MIRPassContext &Ctx) {
  if (!Ctx.Code)
    return false;

  if (!getTargetCodegenInfo(Ctx.TheArch, Ctx.Mode).appendNop(*Ctx.Code)) {
    llvm::WithColor::warning() << "nop pass: unsupported arch\n";
    return false;
  }

  LLVM_DEBUG(llvm::dbgs() << "nop pass: appended NOP to '"
                          << (Ctx.FuncName.empty() ? "<unnamed>" : Ctx.FuncName)
                          << "' (code size now " << Ctx.Code->size()
                          << " bytes)\n");
  return true;
}

} // namespace neverd
