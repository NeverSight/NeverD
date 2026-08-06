//===- MachOPatch.h - Mach-O binary patching --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Mach-O binary patching via new section and trampoline injection.
/// Handles both 32-bit and 64-bit Mach-O via runtime branching.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOPATCH_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOPATCH_H

#include "neverd/backend/codegen/BinaryRewriter.h"

#include "llvm/MC/BinaryRewrite.h"

namespace llvm {
class Module;
}

namespace neverd {

/// Default name of the executable segment NeverD injects into a patched Mach-O.
inline constexpr const char *kDefaultNdTextSegment = "__NDTEXT";

struct MachOPatchOptions : PatchOptionsBase {
  std::string SegmentName = kDefaultNdTextSegment;

  MachOPatchOptions() { SectionName = kNdTextSectionMachO.str(); }
};

class MachOPatcher : public BinaryPatcher {
public:
  PatchResult patch(const std::filesystem::path &InputPath,
                    const std::filesystem::path &OutputPath, llvm::Module &Mod,
                    Arch TargetArch) override;

  PatchResult patch(const std::filesystem::path &InputPath,
                    const std::filesystem::path &OutputPath, llvm::Module &Mod,
                    Arch TargetArch, const MachOPatchOptions &Opts);

  /// Insert a new RX segment before __LINKEDIT (the old __LINKEDIT VA is the
  /// placement VA), shifting __LINKEDIT up and rewriting load commands.
  /// See BinaryPatcher for the contract.
  uint64_t plannedExecSegmentVA(const std::vector<uint8_t> &Binary,
                                Arch TargetArch) override;
  uint64_t appendExecSegment(std::vector<uint8_t> &Binary,
                             llvm::ArrayRef<uint8_t> Code,
                             llvm::StringRef SegName, Arch TargetArch) override;

private:
  struct PatchLayout : PatchLayoutBase {
    uint32_t NCmds = 0;
    uint32_t SizeOfCmds = 0;
    uint32_t HeaderSize = 0;
    uint64_t TextSegVA = 0;
    uint64_t TextSegSize = 0;
    uint64_t TextSegFileOff = 0;
    uint64_t TextSegFileSize = 0;
    uint64_t TextSectVA = 0;
    uint64_t TextSectSize = 0;
    uint64_t TextSectFileOff = 0;
    uint64_t LinkeditVA = 0;
    uint64_t LinkeditFileOff = 0;
    uint32_t LinkeditCmdOff = 0;
  };

  bool parseLayout(const std::vector<uint8_t> &Data, PatchLayout &Layout);
};

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOPATCH_H
