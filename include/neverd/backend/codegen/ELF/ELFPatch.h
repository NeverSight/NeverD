//===- ELFPatch.h - ELF binary patching -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ELF binary patching via new section and trampoline injection.
/// Handles both ELF32 and ELF64 via runtime branching.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_ELF_ELFPATCH_H
#define NEVERD_BACKEND_CODEGEN_ELF_ELFPATCH_H

#include "neverd/ArchSupport.h"
#include "neverd/backend/codegen/BinaryRewriter.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/BinaryRewrite.h"

namespace neverd {

struct ELFPatchOptions : PatchOptionsBase {
  uint32_t SectionFlags = llvm::ELF::SHF_ALLOC | llvm::ELF::SHF_EXECINSTR;
};

class ELFPatcher : public BinaryPatcher {
public:
  PatchResult patch(const std::filesystem::path &InputPath,
                    const std::filesystem::path &OutputPath, llvm::Module &Mod,
                    Arch TargetArch) override;

  /// Append a new PT_LOAD (RX) segment carrying \p Code (a fresh phdr table
  /// is prepended inside the segment, so the code VA is offset past it).
  /// See BinaryPatcher for the contract.
  uint64_t plannedExecSegmentVA(const std::vector<uint8_t> &Binary,
                                Arch TargetArch) override;
  uint64_t appendExecSegment(std::vector<uint8_t> &Binary,
                             llvm::ArrayRef<uint8_t> Code,
                             llvm::StringRef SegName, Arch TargetArch) override;

private:
  struct PatchLayout : PatchLayoutBase {
    uint64_t EntryPoint = 0;
    uint64_t PageSize = kPageSize4K;
    uint16_t PhNum = 0;
    uint64_t PhOff = 0;
  };

  bool parseLayout(const std::vector<uint8_t> &Data, PatchLayout &Layout);
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_ELF_ELFPATCH_H
