//===- MachORegisterResults.h - Native register entries -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bind decoded physical register operands and unwind rows to patch-mode LLVM
/// bodies.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOREGISTERRESULTS_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOREGISTERRESULTS_H

#include "neverd/Common.h"
#include "neverd/loader/LanguageEHDwarf.h"
#include "neverd/loader/MachO/FunctionBody.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace llvm {
class Module;
} // namespace llvm

namespace neverd {
struct BinaryImage;
struct CompactUnwindEntry;
struct ExceptionFunction;
struct ItaniumEHInfo;
struct LowFunc;
struct MedFunc;

/// The Darwin frame record and its descending compact save slots at entry.
llvm::SmallVector<CFIInstruction>
machOCompactFrameRows(const CompactUnwindEntry &Compact);

/// Complete-prologue DWARF for a frameless compact recipe with saved pairs.
/// Darwin's system compact walker cannot restore that layout correctly.
/// Both the native producer and final-object verifier use this contract.
std::vector<CFIInstruction>
machOFramelessSpillRows(const CompactUnwindEntry &Compact);

/// A complete C++/Objective-C frame whose language table can be preserved or
/// rebuilt with a function-relative landing-pad base. Publication separately
/// proves the installed instruction, CFI and landing-pad offsets.
bool canPublishMachOLanguageFrame(const BinaryImage &Image,
                                  const ExceptionFunction &Frame);

/// A function-relative table in an original data section can be referenced
/// directly. A table in a sectionless published segment needs a new owner.
bool canBindMachOLanguageTable(const BinaryImage &Image,
                               const ItaniumEHInfo &Table);

/// Physical SP addressing and simultaneous signing-PC/source-address uses need
/// their machine register body before LLVM resolves an address-valued PHI.
bool needsMachONativeRegisterBody(const MedFunc &Function,
                                  const BinaryImage &Image, const LowFunc *CFG);

/// Sorted FunctionStarts bound source storage when a published image has
/// forwarding entries beyond the original Mach-O function starts.
bool needsMachONativeRegisterBody(const MedFunc &Function,
                                  const BinaryImage &Image, const LowFunc *CFG,
                                  llvm::ArrayRef<va_t> FunctionStarts);

/// Lower audited patch-mode entries that use Darwin variadic cursors, indexed
/// SP accesses, ordinary indexed/writeback memory, scalar FP/NEON data
/// operations, compact FP/frameless saved frames (including PAuth_LR FP
/// recipes) or X8/X9 results consumed by an
/// image-local direct caller,
/// including across local control-flow edges.
/// Keep physical operands, control flow and unwind rows in one native body.
/// Return an error if any declaration withheld for a physical SP access
/// cannot be defined; the caller must discard the incomplete module.
llvm::Error
lowerMachORegisterResults(llvm::Module &Module, const BinaryImage &Image,
                          llvm::ArrayRef<va_t> AdmittedEntries,
                          llvm::ArrayRef<va_t> RequiredNativeEntries,
                          llvm::ArrayRef<LowFunc> SourceFunctions = {});

enum class MachOAddressForm { Single, PagePair, Expanded };

/// Direct entries selecting one copy of an authenticator. The first copy also
/// serves the ordinary fallthrough edge.
struct MachOSigningRoute {
  va_t Signer;
  llvm::SmallVector<va_t, 1> Branches;
  /// Word offset of a distant route's authenticator from its installed signer.
  unsigned IslandWord = 0;
  /// Added words at the signer, including its shared fallthrough branch once.
  unsigned IslandWords = 0;
};

/// An AUT or authenticated return which must consume the installed signing PC.
struct MachOSigningConsumer {
  va_t Instruction;
  /// An ADR sets this GPR for the register form.
  std::optional<unsigned> ModifierRegister;
  /// Rematerialize the input value in its own register after a fallthrough
  /// authentication when all Darwin scratch registers are live.
  bool RestoreSource = false;
  /// Each predecessor sets the register before its own numeric address.
  bool PrepareAtSource = false;

  /// Each additional route repeats the shared prefix and authenticates with
  /// its own signing PC. Only a fallthrough AUT needs a branch to the join.
  llvm::SmallVector<MachOSigningRoute, 0> Routes{};
  bool Returns = false;
  unsigned PrefixWords = 0;

  unsigned routeWords() const { return PrefixWords + (Returns ? 1 : 2); }
  unsigned routeEntry(unsigned Index) const {
    return Index * routeWords() - PrefixWords;
  }
  unsigned insertedWords() const {
    if (!Routes.empty())
      return (Routes.size() - 1) * routeWords();
    return ModifierRegister ? 1 + restoreWords() : 0;
  }
  unsigned restoreWords() const { return RestoreSource ? 2 : 0; }
};

/// A numeric source address produced by a PC-relative instruction or pair.
/// Instruction and target use the input image's preferred addresses.
struct MachOLiftedSourceAddress {
  va_t Instruction;
  va_t Target;
  MachOAddressForm Form = MachOAddressForm::Single;
  /// Byte position of the instruction in the verified inline assembly.
  uint64_t AssemblyOffset = 0;
  /// Inserting a word here preserves the body's local PC-relative encodings.
  bool CanExpand = true;
  std::optional<MachOSigningConsumer> Authentication;

  va_t signingExpansionSite() const {
    return Authentication->PrepareAtSource ? Instruction
                                           : Authentication->Instruction;
  }

  template <typename Fn> void forEachSigningExpansion(Fn Visit) const {
    if (!Authentication)
      return;
    if (const auto Words = Authentication->insertedWords())
      Visit(signingExpansionSite(), Words);
    for (const auto &Route : Authentication->Routes)
      if (Route.IslandWords)
        Visit(Route.Signer, Route.IslandWords);
  }

  bool expandsAuthentication() const {
    return Authentication &&
           (Authentication->insertedWords() || !Authentication->Routes.empty());
  }
};

/// Check each register body and its symbolic call operands against the image
/// before submitting its inline assembly to MC. Returns the original entries
/// for the separate verification of the final linked machine instructions.
/// On success RecoveredData receives sorted, independently proved source
/// ranges absent from the image's data-in-code table. SourceAddresses receives
/// the sorted instruction sites that materialize original image addresses.
llvm::Expected<llvm::DenseSet<va_t>> validateMachORegisterResults(
    llvm::Module &Module, const BinaryImage &Image,
    std::vector<MachODataInCodeRange> *RecoveredData = nullptr,
    std::vector<MachOLiftedSourceAddress> *SourceAddresses = nullptr);

/// Also return the unclassified storage retained in the original image after
/// each entry's reachable code.
llvm::Expected<llvm::DenseSet<va_t>> validateMachORegisterResults(
    llvm::Module &Module, const BinaryImage &Image,
    std::vector<MachODataInCodeRange> *RecoveredData,
    std::vector<MachOLiftedSourceAddress> *SourceAddresses,
    llvm::DenseMap<va_t, ExceptionAddressRange> *RetainedTails);
} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOREGISTERRESULTS_H
