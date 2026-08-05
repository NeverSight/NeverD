//===- HighCEmitter.h - High IR to C emitter -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Emitter that translates HighIR to structured C source code.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_HIGHC_HIGHCEMITTER_H
#define NEVERD_BACKEND_C_HIGHC_HIGHCEMITTER_H
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace neverd {

class HighCEmitter {
public:
  bool emit(const std::vector<HighFunc> &Funcs, llvm::raw_ostream &OS,
            const CEmitterOptions &Opts = {}, DebugContext *Dbg = nullptr);

  bool emitToFile(const std::vector<HighFunc> &Funcs, const std::string &Path,
                  const CEmitterOptions &Opts = {},
                  DebugContext *Dbg = nullptr);
};

} // namespace neverd

#endif // NEVERD_BACKEND_C_HIGHC_HIGHCEMITTER_H
