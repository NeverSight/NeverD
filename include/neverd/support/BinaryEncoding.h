//===- BinaryEncoding.h - Common binary encoding utilities ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Low-level binary reading / writing / alignment helpers shared across
/// loader, codegen, and analysis layers.  Format-neutral — no dependency
/// on COFF, ELF, or Mach-O specifics.
///
/// Follows the same conventions as llvm/Support/Endian.h and
/// llvm/Support/Alignment.h.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_BINARYENCODING_H
#define NEVERD_SUPPORT_BINARYENCODING_H

#include "neverd/Common.h"

#include "llvm/Support/Endian.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace neverd {

// ===--------------------------------------------------------------------===//
// Endian-neutral read / write (assumes LE host, matching LLVM's default)
// ===--------------------------------------------------------------------===//

template <typename T> T readLE(const uint8_t *P) {
  T Val{};
  std::memcpy(&Val, P, sizeof(T));
  return Val;
}

/// Read a fixed-width integer in the byte order declared by its image.
template <typename T> T readTargetEndian(const uint8_t *P, bool LittleEndian) {
  return llvm::support::endian::read<T>(
      P, LittleEndian ? llvm::endianness::little : llvm::endianness::big);
}

template <typename T> void writeLE(uint8_t *P, T Val) {
  std::memcpy(P, &Val, sizeof(T));
}

// ===--------------------------------------------------------------------===//
// Alignment
// ===--------------------------------------------------------------------===//

inline uint64_t alignUp(uint64_t Val, uint64_t Alignment) {
  if (Alignment == 0)
    return Val;
  return (Val + Alignment - 1) & ~(Alignment - 1);
}

// ===--------------------------------------------------------------------===//
// Bounds checking for untrusted inputs
// ===--------------------------------------------------------------------===//

/// True if the byte range [Off, Off + Len) lies entirely within a buffer of
/// \p Size bytes, evaluated without overflow.  Offsets and lengths parsed from
/// untrusted binary headers can sit close to the integer maximum, so the naive
/// `Off + Len <= Size` test can wrap around and wrongly admit an out-of-bounds
/// access; rearranging to `Len <= Size - Off` (guarded by `Off <= Size`, which
/// keeps the subtraction well defined) stays exact for every input.
inline bool rangeInBounds(uint64_t Off, uint64_t Len, uint64_t Size) {
  return Off <= Size && Len <= Size - Off;
}

// ===--------------------------------------------------------------------===//
// Pointer size helper
// ===--------------------------------------------------------------------===//

/// Return the target pointer size in bytes for a given bitness flag.
inline uint32_t getPointerSize(bool Is64) {
  return Is64 ? sizeof(uint64_t) : sizeof(uint32_t);
}

/// Read a pointer-sized value from \p P (4 or 8 bytes depending on \p Is64).
inline uint64_t readPtr(const uint8_t *P, bool Is64, bool LittleEndian = true) {
  const auto Order =
      LittleEndian ? llvm::endianness::little : llvm::endianness::big;
  return Is64 ? llvm::support::endian::read64(P, Order)
              : llvm::support::endian::read32(P, Order);
}

/// Write a pointer-sized value to \p P (4 or 8 bytes depending on \p Is64).
inline void writePtr(uint8_t *P, uint64_t Val, bool Is64,
                     bool LittleEndian = true) {
  const auto Order =
      LittleEndian ? llvm::endianness::little : llvm::endianness::big;
  if (Is64)
    llvm::support::endian::write64(P, Val, Order);
  else
    llvm::support::endian::write32(P, static_cast<uint32_t>(Val), Order);
}

// ===--------------------------------------------------------------------===//
// ARM Thumb bit clearing
// ===--------------------------------------------------------------------===//

/// Clear the Thumb interworking bit from an ARM address.
/// ARM function pointers may have bit 0 set to indicate Thumb mode;
/// clear it to get the actual instruction address.
inline uint64_t clearThumbBit(uint64_t Addr) { return Addr & ~uint64_t(1); }

inline uint64_t normalizeCodeAddress(uint64_t Addr, Arch TargetArch,
                                     InstructionMode Mode) {
  const bool TaggedARM =
      TargetArch == Arch::ARM && (Mode == InstructionMode::Thumb ||
                                  Mode == InstructionMode::MixedARMThumb);
  const bool TaggedMIPS = isMIPS(TargetArch) && hasCompressedMIPSCode(Mode);
  return TaggedARM || TaggedMIPS ? Addr & ~uint64_t(1) : Addr;
}

inline uint64_t serializeCodePointer(uint64_t Addr, Arch TargetArch,
                                     InstructionMode Mode) {
  if (!isSingleInstructionMode(Mode))
    return InvalidVA;
  return (TargetArch == Arch::ARM && Mode == InstructionMode::Thumb) ||
                 (isMIPS(TargetArch) && isMIPSCompressedMode(Mode))
             ? (Addr & ~uint64_t(1)) | uint64_t(1)
             : Addr;
}

// ===--------------------------------------------------------------------===//
// Fixed-length name field readers
// ===--------------------------------------------------------------------===//

/// Field sizes matching llvm/BinaryFormat/COFF.h and MachO.h.
constexpr uint32_t kCOFFNameSize = 8;
constexpr uint32_t kMachONameSize = 16;

inline std::string readFixedName(const char *Buf, size_t MaxLen) {
  return std::string(Buf, strnlen(Buf, MaxLen));
}
inline std::string readCOFFName(const char *Buf) {
  return readFixedName(Buf, kCOFFNameSize);
}
inline std::string readMachOName(const char *Buf) {
  return readFixedName(Buf, kMachONameSize);
}

// ===--------------------------------------------------------------------===//
// COFF/PE layout constants used by both loader and codegen
// ===--------------------------------------------------------------------===//

/// COFF section alignment field: bits 20..23 of Characteristics.
constexpr uint32_t kCOFFAlignShift = 20;

/// Default PE file alignment (512 bytes / 0x200).
constexpr uint32_t kPEDefaultFileAlignment = 0x200;

/// Base relocation entry: high 4 bits = type, low 12 bits = page offset.
constexpr uint32_t kBaseRelocOffsetBits = 12;
constexpr uint32_t kBaseRelocOffsetMask = 0xFFF;

} // namespace neverd

#endif // NEVERD_SUPPORT_BINARYENCODING_H
