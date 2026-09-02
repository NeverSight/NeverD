//===- MachOSignaturePolicy.h - Strict Mach-O signing policy -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Classifies embedded Mach-O signatures before a transactional rewrite.
/// Structural ambiguity is an error.  A valid signature is eligible for
/// automatic ad-hoc re-signing only when every slice is identityless, carries
/// no strong or unknown metadata, and has the same non-empty identifier.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOSIGNATUREPOLICY_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOSIGNATUREPOLICY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::macho_signature {

enum class Kind : uint8_t {
  Unsigned = 0,
  IdentitylessAdHoc = 1,
  Entitled = 2,
  Hardened = 3,
  DeveloperSigned = 4,
};

struct SliceProfile {
  uint32_t CPUType = 0;
  uint32_t CPUSubtype = 0;
  Kind SignatureKind = Kind::Unsigned;
  uint32_t CodeDirectoryVersion = 0;
  uint32_t CodeDirectoryFlags = 0;
  uint8_t CodeDirectoryHashType = 0;
  uint8_t CodeDirectoryHashSize = 0;
  uint8_t CodeDirectoryPageSize = 0;
  uint32_t CodeDirectorySpecialSlots = 0;
  uint64_t ExecSegmentFlags = 0;
  bool LinkerSigned = false;
  bool HasUnsupportedFlags = false;
  bool HasUnsupportedSlots = false;
  bool HasCanonicalCodesignOutputShape = false;
  std::string Identifier;

  bool operator==(const SliceProfile &) const = default;
};

struct Profile {
  bool Universal = false;
  std::vector<SliceProfile> Slices;

  bool operator==(const Profile &) const = default;
};

/// Parse and classify every thin or universal Mach-O slice.
///
/// Malformed containers, load commands, SuperBlobs, CodeDirectories, or
/// conflicting identities return Error.  Unknown slots remain visible in a
/// valid profile and therefore fail automatic re-sign eligibility.
llvm::Expected<Profile> inspect(llvm::ArrayRef<uint8_t> Binary);

/// True only for a profile that can be re-signed with an identityless ad-hoc
/// signature without discarding entitlements, hardened-runtime policy,
/// developer identity, requirements, tickets, or unknown future metadata.
bool canTransactionallyAdHocResign(const Profile &Value);

/// True only for the canonical identityless signature emitted by Apple's
/// codesign tool: one primary CodeDirectory, an exact empty requirements
/// vector, and an exact empty CMS wrapper with the requirements hash bound in
/// special slot two.  This is intentionally distinct from input eligibility.
bool isCanonicalCodesignAdHocOutput(const Profile &Value);

/// Verify that a temporary artifact was re-signed without changing slice
/// identity or introducing/removing any strong or unknown signing metadata.
llvm::Error validateTransactionallyAdHocResigned(const Profile &Before,
                                                 const Profile &After);

} // namespace neverd::macho_signature

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOSIGNATUREPOLICY_H
