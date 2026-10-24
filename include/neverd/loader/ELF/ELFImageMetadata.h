//===- ELFImageMetadata.h - Loader-owned process image facts ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_ELF_ELFIMAGEMETADATA_H
#define NEVERD_LOADER_ELF_ELFIMAGEMETADATA_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {
/// OS identities assigned to the first word of a GNU ABI-tag descriptor.
enum class GNUABISystem : uint32_t {
  Linux = 0,
  Hurd = 1,
  Solaris = 2,
  FreeBSD = 3,
  NetBSD = 4,
  Syllable = 5,
  NaCl = 6
};

/// Decoded program-header fields in file order, including non-load records.
/// Consumers use these facts for OS policy without reparsing image bytes.
struct ELFProgramHeader {
  uint32_t Type, Flags;
  uint64_t FileOffset, VirtualAddress, FileSize, MemorySize, Alignment;
};

struct ELFImageMetadata {
  uint16_t Type;
  uint8_t OSABI, ABIVersion;
  uint32_t Flags;
  uint64_t ProgramHeaderFileOffset;
  uint16_t ProgramHeaderEntrySize;
  std::vector<ELFProgramHeader> ProgramHeaders;
  /// e_ehsize: the file header's own size.
  uint16_t HeaderSize = 0;
  /// The dynamic linker PT_INTERP asks for, empty for none: which C
  /// library a Linux program runs on (/lib64/ld-linux-x86-64.so.2,
  /// /system/bin/linker64, /lib/ld-musl-x86_64.so.1).
  std::string Interpreter;
  /// The Android API level an NDK-built image's identification note
  /// names, none without the note.
  std::optional<uint32_t> AndroidApiLevel;
  /// The FreeBSD version (__FreeBSD_version) the ABI tag note of an image
  /// built for FreeBSD names, none without the note.
  std::optional<uint32_t> FreeBSDVersion;
  /// The OS word of a GNU ABI-tag note (0 is Linux). Multiple different OS
  /// words cannot establish a target, regardless of note order.
  std::optional<uint32_t> GNUABISystem;
  bool ConflictingGNUABISystems = false;
};
} // namespace neverd
#endif
