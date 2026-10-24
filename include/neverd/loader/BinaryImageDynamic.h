//===- BinaryImageDynamic.h - Dynamic linking metadata ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What an image says about how it is to be loaded and bound: its dependencies
/// and initializer arrays, its build identity, and the PE load-configuration
/// provenance the guard tables hang off.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_BINARYIMAGEDYNAMIC_H
#define NEVERD_LOADER_BINARYIMAGEDYNAMIC_H

#include "neverd/Common.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// Linker-authored identity shared by a PE CodeView record and its PDB Info
/// stream.  RSDS (VC7+) uses a GUID; NB10 (VC6 / PDB 2.00) uses the PE
/// timestamp signature.  GUID bytes stay in serialized order.
enum class PDBIdentityKind : uint8_t { RSDS, NB10 };

struct PDBBuildIdentity {
  PDBIdentityKind Kind = PDBIdentityKind::RSDS;
  std::array<uint8_t, 16> Guid{};
  uint32_t Signature = 0;
  uint32_t Age = 0;

  bool isValid() const {
    if (Age == 0)
      return false;
    if (Kind == PDBIdentityKind::NB10)
      return Signature != 0;
    for (uint8_t Byte : Guid)
      if (Byte != 0)
        return true;
    return false;
  }

  bool operator==(const PDBBuildIdentity &Other) const = default;
};

/// Result of reducing every PE CodeView entry to one build identity.  A
/// malformed entry and two different well-formed identities are both
/// Ambiguous: neither may be repaired by a later record or by container order.
enum class PDBIdentityState : uint8_t { Absent, Unique, Ambiguous };

// ===--------------------------------------------------------------------===//
// DynamicInfo — dynamic linking metadata
// ===--------------------------------------------------------------------===//

struct DynamicInfo {
  /// Original PE header facts, also retained by restricted loads without Raw.
  /// No subsystem is invented for a relocatable COFF object.
  std::optional<uint16_t> PESubsystem;
  uint16_t PECharacteristics = 0;
  /// Original Mach-O file type, distinct from its build platform.
  std::optional<uint32_t> MachOFileType;
  std::string SOName;
  std::vector<std::string> NeededLibs;
  std::vector<std::string> RPaths;
  va_t InitAddr = 0;
  va_t FiniAddr = 0;
  std::vector<va_t> PreinitArray;
  std::vector<va_t> InitArray;
  std::vector<va_t> FiniArray;
  /// ELF DT_PLTGOT: the table a PLT entry reaches its import's slot in.  An
  /// i386 PIC PLT entry addresses that slot from this base, held in EBX.
  va_t PltGotAddr = 0;

  /// PDB path (PE/COFF) or build-id string (ELF).
  std::string PDBPath;

  /// PE/COFF CodeView identity (RSDS or NB10).  The optional is populated
  /// exactly when CodeViewPDBIdentityState is Unique.  PDBPath remains a
  /// discovery hint and is never evidence that a companion belongs to this
  /// image.
  PDBIdentityState CodeViewPDBIdentityState = PDBIdentityState::Absent;
  std::optional<PDBBuildIdentity> CodeViewPDBIdentity;

  /// Mach-O: 16-byte UUID from LC_UUID, hex-encoded.
  std::string UUID;

  /// Mach-O: LC_BUILD_VERSION min OS version string (e.g. "14.0.0").
  std::string MinOSVersion;

  /// Mach-O: the platforms (llvm::MachO::PlatformType) its build version
  /// commands name, in order (machOCommandPlatform, MachOPlatform.h).
  std::vector<uint32_t> MachOPlatforms;

  /// PE/COFF: security cookie RVA from Load Configuration directory.
  va_t SecurityCookieRVA = 0;

  /// PE/COFF: CF Guard check function pointer RVA.
  va_t GuardCFCheckFunctionRVA = 0;

  /// PE/COFF load-configuration provenance and guard tables.  Native table
  /// pointers are normalized from VAs to RVAs at load time.
  va_t LoadConfigRVA = 0;
  uint32_t LoadConfigSize = 0;
  uint32_t GuardFlags = 0;
  va_t GuardCFFunctionTableRVA = 0;
  uint64_t GuardCFFunctionCount = 0;
  va_t GuardEHContinuationTableRVA = 0;
  uint64_t GuardEHContinuationCount = 0;
};

} // namespace neverd

#endif // NEVERD_LOADER_BINARYIMAGEDYNAMIC_H
