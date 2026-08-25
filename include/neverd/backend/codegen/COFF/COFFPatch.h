//===- COFFPatch.h - COFF/PE binary patching ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// COFF/PE binary patching via new section and trampoline injection.
/// Handles both PE32 and PE32+ via runtime branching.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFPATCH_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFPATCH_H

#include "neverd/backend/codegen/BinaryRewriter.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/BinaryRewrite.h"

#include <map>

namespace neverd {

struct COFFExceptionPatchPlan;

struct COFFPatchOptions : PatchOptionsBase {
  uint32_t SectionCharacteristics = llvm::COFF::IMAGE_SCN_CNT_CODE |
                                    llvm::COFF::IMAGE_SCN_MEM_EXECUTE |
                                    llvm::COFF::IMAGE_SCN_MEM_READ;
};

class COFFPatcher : public BinaryPatcher {
public:
  PatchResult patch(const std::filesystem::path &InputPath,
                    const std::filesystem::path &OutputPath, llvm::Module &Mod,
                    Arch TargetArch) override;

  /// Append a new RX section (".ndtext") carrying \p Code at the end of the
  /// image.  See BinaryPatcher for the contract.
  uint64_t plannedExecSegmentVA(const std::vector<uint8_t> &Binary,
                                Arch TargetArch) override;
  uint64_t appendExecSegment(std::vector<uint8_t> &Binary,
                             llvm::ArrayRef<uint8_t> Code,
                             llvm::StringRef SegName, Arch TargetArch) override;

protected:
  /// Replace a lifted source-frame cookie call with the compiler-owned check
  /// required by the validated native FH4+GS rewrite contract.  Source
  /// preparation must already have externalized the preserved runtime helper.
  static bool normalizeCompilerOwnedGSSecurityCheck(
      llvm::Module &Module, Arch TargetArch,
      const COFFExceptionPatchPlan &ExceptionPlan,
      const SourceFunctionPreparation &SourcePreparation, std::string &Detail);

private:
  struct PatchLayout : PatchLayoutBase {
    uint32_t PeOffset = 0;
    uint32_t NumSections = 0;
    uint32_t OptionalHdrSize = 0;
    uint32_t SectionTableOff = 0;
    uint64_t ImageBase = 0;
    uint32_t SectionAlignment = 0;
    uint32_t FileAlignment = 0;
    uint32_t SizeOfImage = 0;
    uint32_t SizeOfHeaders = 0;
    uint64_t EntryPointRva = 0;

    struct ImportEntry {
      std::string Name;
      uint64_t IATRva = 0;
    };
    std::vector<ImportEntry> Imports;
    std::map<std::string, uint64_t> IATMap;
  };

  bool parseLayout(const std::vector<uint8_t> &Data, PatchLayout &Layout);
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_COFF_COFFPATCH_H
