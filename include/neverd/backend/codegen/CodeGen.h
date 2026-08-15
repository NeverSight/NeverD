//===- CodeGen.h - LLVM IR to machine code generation --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the Codegen class that compiles LLVM IR to a relocatable
/// object file, and the CodegenResult structure holding the output.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_CODEGEN_H
#define NEVERD_BACKEND_CODEGEN_CODEGEN_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/BinaryRewrite.h"

#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;

struct PatchResult {
  bool Success = false;
  size_t CodeSize = 0;
  size_t TrampolineCount = 0;
  std::string OutputPath;
};

struct CodegenResult {
  std::vector<uint8_t> ObjectData;

  struct FuncEntry {
    std::string Name;
    uint64_t Offset = 0;
    uint64_t Size = 0;
    uint64_t OriginalVA = 0;
  };
  std::vector<FuncEntry> Functions;

  struct RelocEntry {
    uint64_t Offset = 0;
    std::string Symbol;
    int Type = 0;
    int64_t Addend = 0;
    std::string SectionName;
    uint64_t SectionAddr = 0;
  };
  std::vector<RelocEntry> Relocations;

  bool Success = false;
};

/// LLVM IR -> relocatable object for all four ISAs (see ArchSupport.h).
class Codegen {
public:
  CodegenResult compile(llvm::Module &Mod, Arch TargetArch,
                        BinaryFormat ObjectFormat = BinaryFormat::MachO);

  /// Compile directly to fixed-up image bytes using the caller's address
  /// model, bypassing the relocatable object intermediate format.
  /// Callers must reject ImageValid=false and FunctionRangesValid=false;
  /// ordinary code-generation failure may also return no sections.
  llvm::mc_rewrite::RewriteResult
  compileForRewrite(llvm::Module &Mod, Arch TargetArch,
                    const llvm::mc_rewrite::RewriteOptions &Opts,
                    BinaryFormat ObjectFormat = BinaryFormat::MachO,
                    llvm::StringRef TargetTriple = {});
};

/// Patch pre-pass (PIE/ASLR safety). Rewrites absolute pointer constants
/// `inttoptr (iN <VA> to ptr)` whose value lies inside \p Img into references
/// to an external dso_local `__nd_data_<hex>` global. Codegen then emits
/// PC-relative (ADRP+ADD) addressing that the address model resolves to the
/// original VA, instead of an absolute MOV-immediate that breaks once the PIE
/// image is slid by ASLR. Mirrors how the emitter already symbolizes
/// `counter`-style data references.
void symbolizeImageAbsolutePointers(llvm::Module &Mod, const BinaryImage &Img);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_CODEGEN_H
