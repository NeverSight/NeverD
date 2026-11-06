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

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
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

/// Optional pre-CALL stops, indexed once from already authenticated bindings.
/// Both dispatchers emit a static return at selected labels, not a runtime
/// address lookup or a native-state capture inferred from ordinary C.
class X64FunctionCallSitePauses {
  llvm::DenseSet<va_t> Addresses;

public:
  explicit X64FunctionCallSitePauses(llvm::ArrayRef<X64FunctionCallSite> Sites);
  void declare(llvm::raw_ostream &OS) const;
  bool emit(llvm::raw_ostream &OS, va_t Address, llvm::StringRef Indent) const;
};

/// Validate only requested sites against selected original direct CALLs. No
/// index or decoder is built when no interface requests a call-site bridge.
llvm::Expected<std::vector<X64FunctionCallSite>> bindX64FunctionCallSites(
    const BinaryImage &Image, llvm::ArrayRef<X64FunctionCallInterface> Calls,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units);

/// Append pre-CALL/resume bridges sharing the existing instruction bodies and
/// function wrappers. Bound sites borrow Calls/Units until emission completes.
/// Paged bridges retain pending CALLs until the producer restores their PC/SP;
/// only attempted success begins the callee, and every attempt is charged.
llvm::json::Array
emitX64FunctionCallSites(llvm::raw_ostream &OS,
                         llvm::ArrayRef<X64FunctionCallSite> Sites,
                         bool PagedMemory);

} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_X86_X64FUNCTIONCALLSITES_H
