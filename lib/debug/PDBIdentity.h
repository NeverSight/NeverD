//===- PDBIdentity.h - RSDS build identity validation --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared RSDS identity policy for PDB loading, discovery and name hints.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_PDBIDENTITY_H
#define NEVERD_DEBUG_PDBIDENTITY_H

#include "neverd/loader/BinaryImageDynamic.h"

#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <iterator>

namespace neverd::pdb_loader_detail {

inline llvm::Expected<PDBBuildIdentity>
readRSDSBuildIdentity(const llvm::pdb::InfoStream &Info,
                      const llvm::pdb::DbiStream &DBI,
                      const PDBBuildIdentity *Expected = nullptr) {
  PDBBuildIdentity Identity;
  Identity.Kind = PDBIdentityKind::RSDS;
  const auto Guid = Info.getGuid();
  std::copy(std::begin(Guid.Guid), std::end(Guid.Guid), Identity.Guid.begin());
  // RSDS names the link that wrote DBI. Symbol publishing can advance the
  // Info write counter without relinking; an absent DBI age uses Info's age.
  Identity.Age = DBI.getAge() != 0 ? DBI.getAge() : Info.getAge();
  if (!Identity.isValid())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "pdb: PDB Info stream has an invalid GUID/age");
  if (Expected && Identity != *Expected)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "pdb: PDB Info GUID/age does not match PE CodeView RSDS");
  if (DBI.getAge() > Info.getAge())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "pdb: DBI age is newer than PDB Info age");
  return Identity;
}

} // namespace neverd::pdb_loader_detail

#endif // NEVERD_DEBUG_PDBIDENTITY_H
