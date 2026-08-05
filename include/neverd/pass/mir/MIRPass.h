//===- MIRPass.h - Machine IR pass interface -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the MIRPass interface for operating on machine code after
/// codegen, and MIRPassRunner which executes registered passes over
/// each function in a CodegenResult.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_MIR_MIRPASS_H
#define NEVERD_PASS_MIR_MIRPASS_H

#include "neverd/Common.h"
#include "neverd/backend/codegen/CodeGen.h"

#include "llvm/MC/BinaryRewrite.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

struct MIRPassContext {
  std::vector<uint8_t> *Code = nullptr;
  uint64_t BaseVA = 0;
  Arch TheArch = Arch::Unknown;
  InstructionMode Mode = InstructionMode::Default;
  std::string FuncName;
};

class MIRPass {
public:
  virtual ~MIRPass() = default;
  virtual bool run(MIRPassContext &Ctx) = 0;
  virtual const char *name() const = 0;
};

class MIRPassRunner {
public:
  void addPass(MIRPass *Pass);

  /// Run all passes on each function in the codegen result (legacy path).
  bool runOnCodegen(CodegenResult &CG, Arch TheArch,
                    InstructionMode Mode = InstructionMode::Default);

  /// Run all passes on each function in a RewriteResult.
  /// Uses SymbolAddrs to determine function boundaries within the .text
  /// section.
  bool runOnRewriteResult(
      llvm::mc_rewrite::RewriteResult &RR, Arch TheArch,
      InstructionMode Mode = InstructionMode::Default);

private:
  std::vector<MIRPass *> Passes;
};

} // namespace neverd

#endif // NEVERD_PASS_MIR_MIRPASS_H
