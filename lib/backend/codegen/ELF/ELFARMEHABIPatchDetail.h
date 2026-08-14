//===- ELFARMEHABIPatchDetail.h - ARM EHABI rewrite internals ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation detail of the ARM EHABI table rewrite in
/// `lib/backend/codegen/ELF`.  The encoding constants and helpers declared
/// here are shared between the translation units that make up that rewrite and
/// must not be included from outside this directory.
///
/// The constants restate what the loader's own EHABI reader knows, because
/// that reader keeps them in a private header of its own; the vocabulary they
/// are named in, and the \ref UnwindOperation model they describe, are shared
/// rather than restated.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_ELF_ELFARMEHABIPATCHDETAIL_H
#define NEVERD_LIB_BACKEND_CODEGEN_ELF_ELFARMEHABIPATCHDETAIL_H

#include "neverd/backend/codegen/ELF/ELFARMEHABIPatch.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace neverd {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE elf_arm_ehabi_detail {

/// Every index entry is two words: a function address and its description.
inline constexpr uint64_t kIndexEntrySize = 8;

/// A slot is one word on this target, in every table EHABI defines.
inline constexpr uint64_t kWordSize = 4;

/// The reserved description meaning "this frame may not be unwound through".
inline constexpr uint32_t kCantUnwind = 1;

/// Set in a description word that carries its own descriptor rather than a
/// displacement to one.
inline constexpr uint32_t kCompactBit = 0x80000000u;

/// Bits 30-28 of a compact word select the vendor that defined the remaining
/// bits.  Only zero -- ARM's own -- has ever been defined.
inline constexpr uint32_t kCompactVendorMask = 0x70000000u;

/// Bits 27-24 of a compact word: which personality routine it selects.
inline constexpr uint32_t kCompactIndexShift = 24;

/// The highest personality routine index ARM defined.
inline constexpr uint8_t kMaxPersonalityIndex = 2;

/// Where the count of opcode words past the first sits, which differs between
/// the two models: bits 23-16 of a compact word, bits 31-24 of a generic
/// entry's second word.
inline constexpr uint32_t kCompactExtraWordShift = 16;
inline constexpr uint32_t kGenericExtraWordShift = 24;

/// The count is eight bits wide, so this is what the encoding itself permits.
inline constexpr uint32_t kMaxExtraWords = 0xFF;

/// Opcode bytes the first word of each model carries beside its header fields.
inline constexpr size_t kInlineOpcodeBytes = 3;
inline constexpr size_t kCompactOpcodeBytes = 2;
inline constexpr size_t kGenericOpcodeBytes = 3;

/// `finish`, which is also the padding a descriptor fills its last word with.
inline constexpr uint8_t kFinishOpcode = 0xB0;

/// A `prel31` is a 31-bit signed displacement, so this is how far one reaches.
inline constexpr int64_t kPrel31Min = -(int64_t(1) << 30);
inline constexpr int64_t kPrel31Max = (int64_t(1) << 30) - 1;

llvm::Error patchError(const llvm::Twine &Message);

/// Resolve a `prel31` field: a 31-bit signed displacement from the address of
/// the word that holds it, taken modulo the address size as the ABI defines it
/// and as every unwinder computes it.
uint64_t decodePrel31(uint32_t Word, uint64_t FieldVA);

/// Encode the displacement from \p FieldVA to \p TargetVA as a `prel31`, with
/// the top bit left clear for the field's own meaning.  Fails when either
/// address is not one this target can hold or the two are too far apart, which
/// is the one arithmetic error a moved table can introduce silently.
llvm::Error encodePrel31(uint64_t FieldVA, uint64_t TargetVA, uint32_t &Out);

/// The index word that describes \p Record without a descriptor: the reserved
/// "cannot unwind" value, or personality routine 0 and its three opcodes.
llvm::Error encodeARMEHABIIndexWord(const ELFARMEHABIRecord &Record,
                                    uint32_t &Out);

/// Encode the `.ARM.extab` descriptor \p Record needs, to be placed at
/// \p DescriptorVA -- which the generic model's personality field is relative
/// to, so the bytes are only valid at that address.
llvm::Error encodeARMEHABIDescriptor(const ELFARMEHABIRecord &Record,
                                     uint64_t DescriptorVA,
                                     std::vector<uint8_t> &Out);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE elf_arm_ehabi_detail
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_CODEGEN_ELF_ELFARMEHABIPATCHDETAIL_H
