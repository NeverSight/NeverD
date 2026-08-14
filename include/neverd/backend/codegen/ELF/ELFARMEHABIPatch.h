//===- ELFARMEHABIPatch.h - ARM EHABI unwind-table rewrite -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Rewrites the ARM EHABI unwind tables of a 32-bit ARM ELF image so that a
/// relocated or regenerated function keeps working unwind information.
///
/// EHABI is the model the unwinder actually consults on this target: a 32-bit
/// ARM runtime reaches a frame through `.ARM.exidx`, not through the DWARF
/// records `.eh_frame_hdr` indexes, so regenerating `.eh_frame` into an image
/// that carries an index registers nothing at all.  The two are alternatives,
/// and \ref installELFARMEHABI is the ARM side of the choice \ref
/// installELFEHFrame makes everywhere else.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_ELF_ELFARMEHABIPATCH_H
#define NEVERD_BACKEND_CODEGEN_ELF_ELFARMEHABIPATCH_H

#include "neverd/loader/ExceptionUnwindOp.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace llvm {
class Module;
}

namespace neverd {

struct CompiledImage;

/// How an `.ARM.exidx` entry names the description of its function's frame.
///
/// An index entry is two words and the second one has to say everything, so
/// all but the smallest descriptions live out of line in `.ARM.extab` and the
/// word holds a displacement to them instead.
enum class ELFARMEHABIModel : uint8_t {
  /// The frame may not be unwound through, stated in the index word itself.
  CantUnwind,
  /// Personality routine 0 and the three opcode bytes beside it, with no
  /// `.ARM.extab` record at all.  Ordinary frames fit here, which is what lets
  /// an index be grown in an image whose table has no slack left.
  Inline,
  /// An `.ARM.extab` record naming one of the three ARM-defined routines by
  /// index.  Those routines take the scope descriptors EHABI defines for them
  /// rather than an Itanium LSDA.
  Compact,
  /// An `.ARM.extab` record naming its personality routine by address.  This
  /// is the only model that reaches a language personality, and therefore the
  /// only one whose record carries an LSDA.
  Generic,
};

/// One function's entry in the rewritten index, and the descriptor it needs.
struct ELFARMEHABIRecord {
  /// Address of the function, Thumb bit cleared as the index stores it: the
  /// table is searched by program counter, which never has that bit set.
  uint64_t FunctionVA = 0;
  ELFARMEHABIModel Model = ELFARMEHABIModel::CantUnwind;
  /// Which ARM-defined routine a \ref ELFARMEHABIModel::Compact record selects.
  /// Only 0, 1, and 2 are defined.
  uint8_t PersonalityIndex = 0;
  /// Address of the routine a \ref ELFARMEHABIModel::Generic record names.
  uint64_t PersonalityVA = 0;
  /// Unwind opcodes, in the order the unwinder executes them.  Where they fall
  /// short of the words their record spans they are padded with `finish`, as
  /// the encoding defines and as every toolchain writes them.
  std::vector<uint8_t> Opcodes;
  /// The personality routine's own data, which for every language personality
  /// on this target is an Itanium LSDA.  EHABI gives it no section of its own:
  /// it is simply whatever follows the opcodes.
  std::vector<uint8_t> HandlerData;
  /// Address of a descriptor the image already carries, for a record whose
  /// `.ARM.extab` bytes were placed by codegen rather than appended here.  The
  /// content fields above then describe bytes that already exist, and the
  /// entry only has to point at them.
  std::optional<uint64_t> PlacedDescriptorVA;
};

/// The two file-backed regions that carry unwind information on 32-bit ARM.
///
/// Only the table is appended to.  `.ARM.exidx` must stay sorted by function
/// address, so an entry for a function that sorts before an existing one has
/// to be inserted rather than appended -- and every entry it displaces moves,
/// which invalidates the `prel31` displacements those entries are built from.
/// The whole index is therefore re-encoded in place at its original address,
/// into whatever slack follows it.
struct ELFARMEHABIRegion {
  /// `.ARM.exidx`: the entries themselves, and how far the rewrite may run.
  uint64_t IndexVA = 0;
  uint64_t IndexFileOff = 0;
  uint64_t IndexSize = 0;
  /// First file offset the grown index may not reach: the next section, or the
  /// end of the loadable segment that maps it.
  uint64_t IndexLimitFileOff = 0;
  uint64_t IndexSectionHeaderOff = 0;
  /// The `PT_ARM_EXIDX` that publishes the index.  It is what
  /// `PT_GNU_EH_FRAME` is on every other target: an index the program headers
  /// do not name is one the unwinder cannot find, however correctly it is
  /// written.  A statically linked image that resolved `__exidx_start` and
  /// `__exidx_end` into its own code at link time keeps that baked-in range,
  /// which no rewrite of the tables can reach.
  uint64_t ExidxPhdrOff = 0;

  /// `.ARM.extab`: the descriptors, and the slack an appended one has to fit.
  /// Absent when the image ships no table, which leaves only the models that
  /// need no descriptor at all.
  bool HasTable = false;
  uint64_t TableVA = 0;
  uint64_t TableFileOff = 0;
  uint64_t TableSize = 0;
  uint64_t TableLimitFileOff = 0;
  uint64_t TableSectionHeaderOff = 0;
};

/// Locate the EHABI tables of \p Binary and the slack each may grow into.
///
/// Returns nullopt for anything that is not a 32-bit little-endian ARM ELF
/// carrying exactly one `SHT_ARM_EXIDX` section published by a matching
/// `PT_ARM_EXIDX`, which is the shape every linker produces and the only one
/// whose registration this rewrite can guarantee.
std::optional<ELFARMEHABIRegion>
findELFARMEHABIRegion(llvm::ArrayRef<uint8_t> Binary);

/// Re-encode \p Operations as the EHABI opcode byte program that describes the
/// same frame.
///
/// This is the inverse of the decoding the loader performs, over the same
/// normalized vocabulary: a record read out of an image, carried as \ref
/// UnwindOperation, and written back through here describes the frame it
/// started as.  Operations that no EHABI opcode expresses fail closed rather
/// than being approximated, because an unwind program that is nearly right
/// faults where no program at all would merely refuse.
llvm::Expected<std::vector<uint8_t>>
encodeARMEHABIUnwindOpcodes(llvm::ArrayRef<UnwindOperation> Operations);

/// Merge \p Records into the index at \p Region, appending a descriptor to
/// `.ARM.extab` for each record that needs one.
///
/// A record whose function already has an entry replaces that entry, which is
/// what a rewritten-in-place function needs; every other record is inserted so
/// that the index stays sorted.  Nothing is written until the grown index, the
/// appended descriptors, and every `prel31` displacement between them are all
/// known to fit, so a rejected install leaves the image exactly as it was.
llvm::Error
installELFARMEHABIRecords(std::vector<uint8_t> &Binary,
                          const ELFARMEHABIRegion &Region,
                          llvm::ArrayRef<ELFARMEHABIRecord> Records);

/// True when \p Compiled carries a regenerated `.ARM.exidx` fragment, which is
/// what makes EHABI rather than `.eh_frame` the model an ELF patch installs.
bool hasGeneratedELFARMEHABI(const CompiledImage &Compiled);

/// Register the regenerated `.ARM.exidx` in \p Compiled with the index at
/// \p Region.
///
/// Codegen places its own `.ARM.extab` descriptors in the appended segment, so
/// the entries only have to point at them; what the image cannot supply is a
/// place in its own sorted index for the functions they describe, which is
/// what this adds.  Generated fragments are additive and may not reuse an
/// input function key; an intentional in-place replacement goes through
/// \ref installELFARMEHABIRecords, whose record names that operation
/// explicitly. Fails closed when \p Mod carries an exception contract and \p
/// Region is absent, too small, or shaped in a way the rewrite does not model.
/// Source-frame CFI is a required contract even without a language personality,
/// and every required function must have a generated index entry.  A rejected
/// install leaves \p Binary unchanged.
llvm::Error installELFARMEHABI(std::vector<uint8_t> &Binary,
                               const std::optional<ELFARMEHABIRegion> &Region,
                               const CompiledImage &Compiled,
                               const llvm::Module &Mod);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_ELF_ELFARMEHABIPATCH_H
