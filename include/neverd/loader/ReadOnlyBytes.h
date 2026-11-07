//===- ReadOnlyBytes.h - Immutable image storage queries -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate mapped bytes and typed pointer storage independently of use.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_READONLYBYTES_H
#define NEVERD_LOADER_READONLYBYTES_H

#include "neverd/Common.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;

/// Whether this linked native image has a supported immutable-read contract.
/// Individual reads still require exact mappings, permissions and fixups.
bool supportsImmutableImageReads(const BinaryImage &Image);

/// Read one uniquely mapped immutable byte range with no pointer fixups.
/// The caller separately proves that copying its contents preserves the use;
/// this routine establishes neither pointer identity nor ownership/lifetime.
std::optional<std::vector<uint8_t>>
readImmutableImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size);

/// Read an initial structural byte range with unique storage and no fixups.
/// Writable records are permitted only as initializer evidence. This is not a
/// stable runtime value or permission to emit a copied object; declaration
/// owners must authenticate the record and retain dynamic accesses separately.
std::optional<std::vector<uint8_t>>
readInitialImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size);

/// Read uniquely mapped immutable instruction bytes without fixups. This
/// proves byte availability only, not function ownership or instruction
/// semantics. Ordinary data reads continue to reject executable addresses.
std::optional<std::vector<uint8_t>>
readImmutableCodeBytes(const BinaryImage &Image, va_t Address, uint32_t Size);

/// Read a little-endian scalar (one through eight bytes) using the immutable
/// data/code readers. Full pointer-width values that could be image addresses
/// remain unknown: raw bytes do not establish a relocatable pointer identity.
/// An optional budget charges metadata inventories, nested ownership queries
/// and result bytes before reading. An unaffordable read leaves the budget
/// intact so callers can continue memory-independent proofs; callers may cache
/// reads only while the image and all of its metadata remain unchanged.
std::optional<uint64_t>
readImmutableImageScalar(const BinaryImage &Image, va_t Address, uint32_t Size,
                         size_t *RemainingWork = nullptr);

/// Prove unique, complete file-backed instruction storage. Code may be
/// writable, execute-only or contain fixups; this establishes neither
/// immutability nor a callable identity, ABI or instruction semantics.
bool isFileBackedCodeImageRange(const BinaryImage &Image, va_t Address,
                                uint32_t Size);

/// Read immutable PE32 instruction storage while admitting only HIGHLOW
/// relocations at the supplied four-byte absolute operands. The consumer must
/// decode and authenticate every operand; this proves storage and relocation
/// shape, not the instruction's meaning or permission to copy its bytes.
std::optional<std::vector<uint8_t>>
readImmutablePE32CodeBytes(const BinaryImage &Image, va_t Address,
                           uint32_t Size,
                           const std::vector<va_t> &AbsoluteOperands);

/// The same current-storage proof under one cumulative work allowance.
/// Exhaustion returns no bytes and saturates Work at Limit.
std::optional<std::vector<uint8_t>> readImmutablePE32CodeBytes(
    const BinaryImage &Image, va_t Address, uint32_t Size,
    const std::vector<va_t> &AbsoluteOperands, size_t &Work, size_t Limit);

/// Read the complete runtime bits of one exact, resolved Mach-O chained
/// rebase in unique immutable storage. Competing or overlapping fixups are
/// rejected. This grants no ordinary pointer identity or byte-copy authority;
/// a consumer must separately prove the value's representation and relocation.
std::optional<uint64_t> readImmutableChainedImageValue(const BinaryImage &Image,
                                                       va_t Address);

/// Prove a complete store range lies in unique file-backed writable image
/// storage, disjoint from a function's private frame and newly allocated
/// objects. This proves only the storage owner, never its contents or a source
/// binding. Zero-fill, executable and ambiguous mappings are not accepted.
bool isFileBackedWritableImageRange(const BinaryImage &Image, va_t Address,
                                    uint32_t Size);

/// Whether scalar bits could name a source-owned image object or instruction.
/// A pointer requires the target width and a mapped section owner; segment
/// padding and narrow integer pieces do not provide that identity. This check
/// does not authorize byte copies: immutability, fixups and uses are separate.
bool isImagePointerBitPattern(const BinaryImage &Image, uint64_t Bits,
                              uint16_t Width);

/// Read a full-width resolved data pointer from uniquely mapped immutable
/// storage. The relocation must identify the current target's owning range.
/// Supports linked Mach-O and exact PE DIR64/HIGHLOW data slots; conflicting
/// or partial relocations, runtime import slots and duplicate fixups refuse.
/// This proves the loaded value only, not a binding for the slot's address or
/// permission to copy the target object.
std::optional<va_t> readImmutableImagePointer(const BinaryImage &Image,
                                              va_t Address);

/// Read one exact resolved code pointer in unique immutable storage: a linked
/// Mach-O chained slot or a PE DIR64/HIGHLOW slot.
/// The target must be a current authenticated local function entry with
/// immutable instruction bytes. This proves an address, never its ABI or
/// permission to use a code pointer as an ordinary data pointer.
std::optional<va_t> readImmutableImageCodePointer(const BinaryImage &Image,
                                                  va_t Address);

/// Read the preferred bits of one immutable PE32 HIGHLOW code-pointer slot.
/// Unlike an ordinary callable pointer, a runtime table may name an interior
/// callback. Its format owner must authenticate that role separately. This
/// proves exact storage/fixup shape and immutable target bytes, not an ordinary
/// function entry, instruction boundary, callback ABI or permission to copy.
std::optional<va_t>
readImmutablePE32CodePointerStorage(const BinaryImage &Image, va_t Address);

/// One exact PE32 HIGHLOW jump-table slot, including a table in executable
/// storage. This grants only the selected immutable pointer and target bytes;
/// the consumer owns the index, instruction boundary and invocation domain.
/// No table extent or ordinary function entry is inferred.
std::optional<va_t> readImmutablePE32JumpTableTarget(const BinaryImage &Image,
                                                     va_t Address, size_t &Work,
                                                     size_t Limit);

/// Prove one exact strong, zero-addend import slot has unique immutable
/// file-backed storage and no competing fixups. This authenticates the slot,
/// not a runtime object layout, provider export, or permission to copy bytes.
bool isImmutableImageImportSlot(const BinaryImage &Image, va_t Address);

/// Same exact import/fixup proof in possibly writable initializer storage.
/// This authenticates an initial declaration, never a stable runtime pointer
/// or permission to copy the imported object. Provider proof stays separate.
bool isInitialImageImportSlot(const BinaryImage &Image, va_t Address);

/// Authenticate an exact non-delay PE import-directory slot in unique,
/// complete file-backed storage. Competing fixups or bindings refuse. The
/// provider's behavior and preservation of the binding at a call remain the
/// consumer's obligations; this does not make a writable IAT immutable.
bool isFileBackedPEImportSlot(const BinaryImage &Image, va_t Address);

/// Read an immutable scalar ivar-offset slot with its exact loader reference.
/// All other overlapping fixups remain forbidden. The declaration consumer
/// must separately match the referenced class, field, type and layout.
std::optional<uint64_t> readImmutableImageIvarOffset(const BinaryImage &Image,
                                                     va_t Address);

/// Same storage/fixup proof, additionally requiring one exact Class reference
/// matching the strong imported class symbol. Only that matching metadata
/// record is permitted; provider ownership remains the caller's obligation.
bool isImmutableImageClassImportSlot(const BinaryImage &Image, va_t Address);

/// Read the initial value of an authenticated local data-pointer relocation.
/// Storage may be writable: this is an initializer recipe, never permission
/// to replace subsequent loads with the initial value. The caller must rebuild
/// the slot's shared mutable identity and separately validate the target.
std::optional<va_t> readInitialImagePointer(const BinaryImage &Image,
                                            va_t Address);
/// Same initial resolved-pointer proof, admitting only the exact loader
/// selector-reference record at this slot. This does not freeze the runtime
/// SEL and does not prove the pointed-to selector string or dispatch ABI.
std::optional<va_t> readInitialImageSelectorPointer(const BinaryImage &Image,
                                                    va_t Address);
} // namespace neverd
#endif // NEVERD_LOADER_READONLYBYTES_H
