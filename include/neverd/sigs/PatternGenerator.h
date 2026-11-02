//===- PatternGenerator.h - Build .pat lines from object files -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Turns the functions an object file defines into .pat lines, the format
/// PatternParser reads back. Ordinary output is FLIRT-compatible; optional
/// required-reference constraints use NeverD's explicit extension.
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
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
namespace object {
class ObjectFile;
} // namespace object
} // namespace llvm

namespace neverd {
namespace sigs {

struct PatternGeneratorOptions {
  /// Bytes stated one by one before the CRC span.
  unsigned LeadingLen = SignatureLimits::DefaultLeadingBytes;
  /// Functions shorter than this are not written.
  unsigned MinFuncSize = SignatureLimits::DefaultMinFunctionBytes;
  /// Bytes stated one by one after the CRC span. A value at least as large
  /// as a function covers it to its end.
  unsigned TailLen = 0;
  /// State the routines each COFF or ELF function branches to directly as
  /// `^offset name` references (see PatternModule::References). Off by
  /// default: a loader older than the references rejects a line with one.
  /// COFF x86/x64 also states canonical indirect import-pointer references
  /// and unprefixed conditional rel32 references to explicit function symbols
  /// with zero relocation addends. Unsupported forms remain unstated.
  bool EmitReferences = false;
  /// Opt in to typed external-data references in complete contextual/evidence
  /// records. Ordinary FLIRT-compatible output never acquires this extension.
  bool EmitDataReferences = false;
  /// For source-local operands, opt in to exact same-object relations with
  /// these independently matchable functions. The anchor's canonical
  /// data operand, not a static symbol's spelling, identifies the relation.
  std::vector<std::string> LocalDataAnchors;
  /// Also describe signed address deltas within one non-COMDAT data section.
  /// The anchor keeps its exact zero-addend operand; consumers may reference
  /// defined static or external data, including checked relocation addends.
  bool EmitLocalDataLayout = false;
  /// Emit referenced functions with the NeverD `!refs` constraint and full
  /// byte coverage, including functions below the ordinary fixed-byte floor.
  /// Unreferenced functions retain the ordinary floor and output format.
  bool RequireReferences = false;
  /// Emit complete bodies with !caller. Their names may be requested by
  /// verified callers, never by a standalone byte scan. Outgoing references
  /// can reject a body but cannot change its caller or support a new query.
  /// Unlike ordinary patterns, this includes functions below the size floor.
  bool RequireCaller = false;
  /// Emit complete collision evidence without the size/fixed-byte floors.
  /// Includes self references and their aliases: omitting them can hide a
  /// source constraint from an overlapping raw counterclaim.
  /// Every line is marked !evidence and rejected by the matching parser.
  /// It is input to offline supplement validation, never a signature pack.
  bool CollisionEvidence = false;
  /// The names a COFF link resolves a symbol to when no object defines it,
  /// as `/alternatename:symbol=alternate` directives give them (see
  /// collectAlternateNames).  A reference to the symbol also names each
  /// alternate, at the same offset: the branch reaches one of them.
  std::map<std::string, std::vector<std::string>, std::less<>> AlternateNames;
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
  /// (Mach-O CPU type, relocation type) pairs behind UnsupportedRelocation.
  std::set<std::pair<uint32_t, uint32_t>> UnsupportedMachORelocations;
  /// Functions left out because a Mach-O linker optimization hint names an
  /// instruction in them that the linker may rewrite in ways unknown: the
  /// hint's kind or CPU type is one machOOptimizationHintWidth does not know,
  /// or the object's hints cannot be read.
  unsigned UnsupportedHint = 0;
  /// (Mach-O CPU type, hint kind) pairs behind UnsupportedHint.
  std::set<std::pair<uint32_t, uint32_t>> UnsupportedMachOHints;
  /// Mach-O objects whose linker optimization hints cannot be read; each of
  /// their functions counts in UnsupportedHint.
  unsigned UnreadableHints = 0;
  /// Routines a compiler synthesized (isSynthesizedRoutineName), which no line
  /// names.
  unsigned Synthesized = 0;

  PatternGeneratorStats &operator+=(const PatternGeneratorStats &Other);
};

/// The bytes a relocation may leave different in a linked image: its field,
/// and the instruction bytes around it a linker may rewrite.
///
/// A linker does more to ELF and Mach-O code than fill in fields. It relaxes
/// GOT loads (`mov foo@GOTPCREL(%rip)` becomes `lea`, `call
/// *foo@GOTPCREL(%rip)` a direct call), and in a static or executable link an
/// ELF linker replaces whole TLS access sequences. Such a relocation's
/// footprint starts \p Before bytes ahead of the field and spans \p Before +
/// \p Width bytes, the most any form of the instruction or sequence it marks
/// takes.
struct RelocationFootprint {
  unsigned Before = 0;
  unsigned Width = 0;
};
using ELFRelocationFootprint = RelocationFootprint;

/// The footprint of an ELF relocation of \p Type in a relocatable object for
/// \p Machine (an EM_* value): x86 and x86-64, ARM and AArch64. Markers that
/// rewrite nothing have an empty footprint. Returns std::nullopt for a
/// machine or type this table does not know, including the dynamic types a
/// relocatable object does not hold.
std::optional<ELFRelocationFootprint> elfRelocationFootprint(uint16_t Machine,
                                                             uint32_t Type);

/// The footprint of a Mach-O relocation of \p Type, whose r_length is
/// \p Length, in an object for \p CPUType (a CPU_TYPE_* value): arm64 and
/// x86-64. Returns std::nullopt for a CPU type or relocation type this table
/// does not know.
std::optional<RelocationFootprint>
machORelocationFootprint(uint32_t CPUType, uint32_t Type, unsigned Length);

/// The bytes of each instruction a Mach-O linker optimization hint of
/// \p Kind names, all of which the linker may rewrite, in an object for
/// \p CPUType (a CPU_TYPE_* value): the whole instruction on arm64, for the
/// kinds llvm::MCLOHType lists. Returns std::nullopt for a CPU type or kind
/// this table does not know.
std::optional<unsigned> machOOptimizationHintWidth(uint32_t CPUType,
                                                   uint32_t Kind);

/// Whether \p Name is a routine a compiler synthesized and numbered per
/// object, such as LLVM's machine-outlined `OUTLINED_FUNCTION_<n>`, spelled
/// with one reserved `_` in front in a Mach-O object; see LinkerSyntax.def.
/// Such a number says nothing about another build's code, so no line is
/// named after one and no reference names one.
bool isSynthesizedRoutineName(llvm::StringRef Name);

/// An architecture a signature file is made for; see TargetMachine.def.
enum class TargetMachine {
#define NEVERD_SIGS_TARGET_MACHINE(Name, Spelling, COFFMachine, ELFMachine,    \
                                   AddressBytes, MachOCPUType)                 \
  Name,
#include "neverd/sigs/TargetMachine.def"
};

/// The architecture neverd-sigmaker's `--machine` gives \p Name, or
/// std::nullopt.
std::optional<TargetMachine> parseTargetMachine(llvm::StringRef Name);

/// The names `--machine` gives the architectures, in the order of
/// TargetMachine.
llvm::ArrayRef<llvm::StringLiteral> targetMachineNames();

/// Whether \p Obj holds code for \p Machine: a COFF object of that machine,
/// an ELF object of its class and e_machine, or a Mach-O object of its CPU
/// type. An x32 object (ELFCLASS32 with EM_X86_64) is not x64 code, an
/// arm64_32 object not arm64 code, and no other format matches.
bool isObjectForMachine(const llvm::object::ObjectFile &Obj,
                        TargetMachine Machine);

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
/// the linker may rewrite and \p References, sorted by offset, the routines it
/// branches to. Returns false, writing nothing, when the function is shorter
/// than Opts.MinFuncSize or its line would state fewer than
/// SignatureMatcher::MinStatedBytes bytes exactly, unless reference context
/// is explicitly required. Required-reference lines cover the whole function.
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
/// Also accepts canonical FF 15/FF 25 import fields (a single REX.W prefix
/// is allowed in x64): DIR32 on x86, REL32
/// on x64. The caller must separately require an import-pointer symbol.
std::optional<uint64_t> coffBranchReferenceOffset(uint16_t Machine,
                                                  uint16_t Type,
                                                  llvm::ArrayRef<uint8_t> Code,
                                                  uint64_t RelocationOffset,
                                                  uint64_t FunctionOffset);

/// Where an ELF relocation of \p Type states a direct branch, for objects of
/// \p Machine (an EM_* value): the offset a `^offset name` reference names,
/// or std::nullopt.  That is a PC32 or PLT32 field after an E8 or E9 opcode
/// on x86 and x86-64, a CALL26 or JUMP26 instruction on AArch64, and a
/// THM_CALL or THM_JUMP24 (Thumb-2 BL or B.W) instruction on ARM -- or one
/// byte past a CALL, JUMP24, PLT32 or PC24 (ARM-state B or BL) instruction,
/// an odd offset (see PatternModule::References).  The function's bytes are
/// \p Function and \p Offset is the relocation's offset in them; the branch
/// must lie within them.
std::optional<uint64_t>
elfBranchReferenceOffset(uint16_t Machine, uint32_t Type,
                         llvm::ArrayRef<uint8_t> Function, uint64_t Offset);

/// Where a Mach-O relocation of \p Type states a direct branch, for objects
/// of \p CPUType (a CPU_TYPE_* value): the offset a `^offset name` reference
/// names, or std::nullopt.  That is an arm64 BRANCH26 instruction (B or BL),
/// and on x86-64 a BRANCH field after an E8 or E9 opcode.  The function's
/// bytes are \p Function and \p Offset is the relocation's offset in them;
/// the branch must lie within them.
std::optional<uint64_t>
machOBranchReferenceOffset(uint32_t CPUType, uint32_t Type,
                           llvm::ArrayRef<uint8_t> Function, uint64_t Offset);

/// Adds the `/alternatename:symbol=alternate` directives of \p Obj's
/// `.drectve` sections to \p Names, if it is a COFF object.
void collectAlternateNames(
    const llvm::object::ObjectFile &Obj,
    std::map<std::string, std::vector<std::string>, std::less<>> &Names);

/// Complete and incomplete primary x64 COFF table declarations. Incomplete
/// declarations remain in offline evidence and veto publication of that type.
struct VTableEvidenceStats {
  unsigned Complete = 0;
  unsigned Incomplete = 0;
};

/// Write JSONL source declarations, including unsupported layouts and weak
/// bindings. This is offline evidence, never directly loadable signatures.
/// Malformed symbol/section reads fail instead of hiding a counterclaim.
llvm::Expected<VTableEvidenceStats>
generateVTableEvidence(const llvm::object::ObjectFile &Obj,
                       llvm::raw_ostream &OS);

/// Source configuration, requested pointer definitions and actual function
/// definitions. Missing or unsupported declarations remain explicit evidence.
struct RuntimePointerEvidenceStats {
  unsigned Complete = 0;
  unsigned Incomplete = 0;
};
llvm::Expected<RuntimePointerEvidenceStats>
generateRuntimePointerEvidence(const llvm::object::ObjectFile &Obj,
                               llvm::ArrayRef<std::string> Names,
                               llvm::raw_ostream &OS);

/// Complete nonsplittable COFF x64 code contributions and explicit refusals.
/// Every relocation needs a canonical import/shared-data operand. Other
/// formats, COMDAT fragments and missing ownership never become source layouts.
struct CodeLayoutEvidenceStats {
  unsigned Complete = 0;
  unsigned Incomplete = 0;
};
llvm::Expected<CodeLayoutEvidenceStats>
generateCodeLayoutEvidence(const llvm::object::ObjectFile &Obj,
                           llvm::raw_ostream &OS);

/// Writes one .pat line per function \p Obj defines.
///
/// COFF objects are read through their own symbol table: a function is a
/// function-typed external or static symbol in a code section, and it ends
/// where the next such symbol starts or where its section does -- label
/// symbols such as MSVC's `$LN` jump targets do not end it. Relocation widths
/// come from coffRelocationWidth. In ELF and Mach-O objects every function
/// symbol is a function, ending at the next symbol of any kind in its
/// section; the function symbols that label one address are one routine's
/// aliases and share one line, their names in preferredAliasOrder. An ELF
/// object keeps its relocations in sections of their own, each naming the
/// section it applies to; their footprints come from elfRelocationFootprint.
/// An ELF function ends earlier where its symbol's size says so, before the
/// padding that aligns the next one. A Mach-O object's relocations have the
/// footprints machORelocationFootprint gives, and a function holding one it
/// does not know -- any of a CPU type other than arm64 and x86-64 -- is left
/// out. Each instruction a Mach-O linker optimization hint names is left
/// unstated too, as machOOptimizationHintWidth says, and a function holding
/// one it does not know is left out. In an object of any other format every
/// relocation covers four bytes. In every format, a routine a compiler
/// synthesized (isSynthesizedRoutineName) ends the function before it but is
/// neither written nor stated as a reference.
PatternGeneratorStats generatePatterns(const llvm::object::ObjectFile &Obj,
                                       const PatternGeneratorOptions &Opts,
                                       llvm::raw_ostream &OS);

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_PATTERNGENERATOR_H
