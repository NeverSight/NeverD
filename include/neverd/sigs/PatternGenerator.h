//===- PatternGenerator.h - Build .pat lines from object files -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Turns the functions an object file defines into FLIRT-compatible .pat
/// lines, the format PatternParser reads back.
///
/// A signature states the bytes a function has in every image it is linked
/// into. A relocated byte holds a placeholder in the object and an address in
/// the image, so it is written as a wildcard, and the CRC stops at the first
/// one because a checksum cannot express a wildcard. Everything therefore
/// depends on knowing exactly which bytes each relocation rewrites.
///
/// Names are the linkage names the object's symbol table spells, byte for
/// byte: `?Close@CFile@@UEAAXXZ`, `_ZNSt6thread4joinEv`, `_memcpy`. They are
/// never demangled, sanitized, prefixed, or truncated, so a match names a
/// routine the way its own library does.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_PATTERNGENERATOR_H
#define NEVERD_SIGS_PATTERNGENERATOR_H

#include "neverd/sigs/Signature.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>
#include <set>
#include <utility>

namespace llvm {
namespace object {
class ObjectFile;
} // namespace object
} // namespace llvm

namespace neverd {
namespace sigs {

struct PatternGeneratorOptions {
  /// Bytes stated one by one before the CRC span.
  unsigned LeadingLen = 32;
  /// Functions shorter than this are not written.
  unsigned MinFuncSize = 4;
  /// Bytes stated one by one after the CRC span. A value at least as large
  /// as a function covers it to its end.
  unsigned TailLen = 0;
  /// State the routines each COFF function branches to directly as
  /// `^offset name` references (see PatternModule::References). Off by
  /// default: a loader older than the references rejects a line with one.
  bool EmitReferences = false;
};

/// What one object contributed, and what it could not.
struct PatternGeneratorStats {
  unsigned Functions = 0;
  unsigned TooSmall = 0;
  /// Functions left out because their line would state fewer than
  /// SignatureMatcher::MinStatedBytes bytes exactly.
  unsigned TooWeak = 0;
  /// Functions left out because a relocation inside them has a type whose
  /// width is unknown. Writing them would state placeholder bytes as if they
  /// were the function's own.
  unsigned UnsupportedRelocation = 0;
  /// (COFF machine, relocation type) pairs behind UnsupportedRelocation.
  std::set<std::pair<uint16_t, uint16_t>> UnsupportedCOFFRelocations;
  /// (ELF machine, relocation type) pairs behind UnsupportedRelocation.
  std::set<std::pair<uint16_t, uint32_t>> UnsupportedELFRelocations;

  PatternGeneratorStats &operator+=(const PatternGeneratorStats &Other);
};

/// The bytes an ELF relocation may leave different in a linked image: its
/// field, and the instruction bytes around it a linker may rewrite.
///
/// A linker does more to ELF code than fill in fields. It relaxes GOT loads
/// (`mov foo@GOTPCREL(%rip)` becomes `lea`, `call *foo@GOTPCREL(%rip)` a
/// direct call), and in a static or executable link it replaces whole TLS
/// access sequences. Such a relocation's footprint starts \p Before bytes
/// ahead of the field and spans \p Before + \p Width bytes, the most any
/// form of the instruction or sequence it marks takes.
struct ELFRelocationFootprint {
  unsigned Before = 0;
  unsigned Width = 0;
};

/// The footprint of an ELF relocation of \p Type in a relocatable object for
/// \p Machine (an EM_* value): x86 and x86-64, ARM and AArch64. Markers that
/// rewrite nothing have an empty footprint. Returns std::nullopt for a
/// machine or type this table does not know, including the dynamic types a
/// relocatable object does not hold.
std::optional<ELFRelocationFootprint> elfRelocationFootprint(uint16_t Machine,
                                                             uint32_t Type);

/// The number of bytes a COFF relocation of \p Type rewrites at its offset
/// for objects of \p Machine (an IMAGE_FILE_MACHINE_* value).
///
/// Instruction relocations cover the whole instruction they patch -- four
/// bytes on ARM64 and Thumb-2, eight for IMAGE_REL_ARM_MOV32T/MOV32A, which
/// patch a MOVW/MOVT pair -- because pattern bytes cannot wildcard part of a
/// byte. Pair and absolute relocations rewrite nothing and are 0. Returns
/// std::nullopt for a machine or type this table does not know.
std::optional<unsigned> coffRelocationWidth(uint16_t Machine, uint16_t Type);

/// How many bytes the line for a function would state exactly: the fixed
/// ones among the leading bytes and the tail, and every byte of the CRC span.
size_t statedByteCount(llvm::ArrayRef<bool> Wildcard,
                       const PatternGeneratorOptions &Opts);

/// Writes one .pat line for \p Data, the complete bytes of the function
/// called \p Names -- one name, or every linkage name of a routine several
/// symbols label, each at offset 0 -- where \p Wildcard marks the bytes
/// relocations rewrite and \p References, sorted by offset, the routines it
/// branches to. Returns false, writing nothing, when the function is shorter
/// than Opts.MinFuncSize or its line would state fewer than
/// SignatureMatcher::MinStatedBytes bytes exactly.
bool emitPatternLine(llvm::raw_ostream &OS,
                     llvm::ArrayRef<llvm::StringRef> Names,
                     llvm::ArrayRef<uint8_t> Data,
                     llvm::ArrayRef<bool> Wildcard,
                     const PatternGeneratorOptions &Opts,
                     llvm::ArrayRef<FuncRef> References = {});
inline bool emitPatternLine(llvm::raw_ostream &OS, llvm::StringRef Name,
                            llvm::ArrayRef<uint8_t> Data,
                            llvm::ArrayRef<bool> Wildcard,
                            const PatternGeneratorOptions &Opts,
                            llvm::ArrayRef<FuncRef> References = {}) {
  return emitPatternLine(OS, llvm::ArrayRef<llvm::StringRef>(Name), Data,
                         Wildcard, Opts, References);
}

/// Where a COFF relocation states a direct branch: the function offset a
/// `^offset name` reference names, or std::nullopt.  That is a REL32 field
/// after an E8 or E9 opcode on x86 and x64, and a BRANCH26 (ARM64) or
/// BRANCH24T/BLX23T (Thumb-2) instruction.  \p Code holds the section's
/// bytes, \p RelocationOffset and \p FunctionOffset are section offsets.
std::optional<uint64_t> coffBranchReferenceOffset(uint16_t Machine,
                                                  uint16_t Type,
                                                  llvm::ArrayRef<uint8_t> Code,
                                                  uint64_t RelocationOffset,
                                                  uint64_t FunctionOffset);

/// Writes one .pat line per function \p Obj defines.
///
/// COFF objects are read through their own symbol table: a function is a
/// function-typed external or static symbol in a code section, and it ends
/// where the next such symbol starts or where its section does -- label
/// symbols such as MSVC's `$LN` jump targets do not end it. Relocation widths
/// come from coffRelocationWidth. In ELF and Mach-O objects every function
/// symbol is a function, ending at the next symbol of any kind in its
/// section. An ELF object keeps its relocations in sections of their own,
/// each naming the section it applies to; their footprints come from
/// elfRelocationFootprint. The ELF function symbols that label one address
/// are one routine's aliases and share one line, their names in
/// preferredAliasOrder. Mach-O relocations cover four bytes.
PatternGeneratorStats generatePatterns(const llvm::object::ObjectFile &Obj,
                                       const PatternGeneratorOptions &Opts,
                                       llvm::raw_ostream &OS);

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_PATTERNGENERATOR_H
