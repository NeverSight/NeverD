//===- X64FunctionCallSites.h - Original CALL context bridges -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind checked scalar interfaces at their original native CALL instructions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLSITES_H
#define NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLSITES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <vector>

namespace llvm {
class raw_ostream;
}
namespace neverd {
struct BinaryImage;
struct X64FunctionCallInterface;
namespace analysis {
struct X64ContinuationInstructionSource;
}

struct X64FunctionCallSite {
  const X64FunctionCallInterface *Interface = nullptr;
  const analysis::X64ContinuationInstructionSource *Instruction = nullptr;
};

/// Validate only requested sites against selected original direct CALLs. No
/// index or decoder is built when no interface requests a call-site bridge.
llvm::Expected<std::vector<X64FunctionCallSite>> bindX64FunctionCallSites(
    const BinaryImage &Image, llvm::ArrayRef<X64FunctionCallInterface> Calls,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units);

/// Append pre-CALL/resume bridges sharing the existing instruction bodies and
/// function wrappers. Bound sites borrow Calls/Units until emission completes.
llvm::json::Array
emitX64FunctionCallSites(llvm::raw_ostream &OS,
                         llvm::ArrayRef<X64FunctionCallSite> Sites);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLSITES_H
