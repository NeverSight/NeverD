//===- LLVMCEmitter.h - LLVM IR to C emitter -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Emitter that translates LLVM IR to goto-style C source code.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_LLVMC_LLVMCEMITTER_H
#define NEVERD_BACKEND_C_LLVMC_LLVMCEMITTER_H
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/debug/DebugContext.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>

namespace neverd {

struct BinaryImage;

class LLVMCEmitter {
public:
  bool emit(llvm::Module &Mod, llvm::raw_ostream &OS,
            const CEmitterOptions &Opts = {}, DebugContext *Dbg = nullptr,
            const BinaryImage *Img = nullptr);

  bool emitToFile(llvm::Module &Mod, const std::string &Path,
                  const CEmitterOptions &Opts = {}, DebugContext *Dbg = nullptr,
                  const BinaryImage *Img = nullptr);
};

} // namespace neverd

#endif // NEVERD_BACKEND_C_LLVMC_LLVMCEMITTER_H
