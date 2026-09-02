//===- PESignaturePolicy.h - Strict PE signing policy ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_COFF_PESIGNATUREPOLICY_H
#define NEVERD_BACKEND_CODEGEN_COFF_PESIGNATUREPOLICY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>

namespace neverd::pe_signature {

enum class Kind : uint8_t { Unsigned = 0, Authenticode = 1 };

struct Profile {
  Kind SignatureKind = Kind::Unsigned;
  uint32_t CertificateTableOffset = 0;
  uint32_t CertificateTableSize = 0;
  uint32_t CertificateCount = 0;

  bool operator==(const Profile &) const = default;
};

/// Parse the PE Optional Header Security Directory.  The directory's virtual
/// address is a file offset, not an RVA.  A non-empty, structurally complete
/// WIN_CERTIFICATE table is reported as Authenticode; ambiguous encodings are
/// errors rather than unsigned images.
llvm::Expected<Profile> inspect(llvm::ArrayRef<uint8_t> Binary);

} // namespace neverd::pe_signature

#endif // NEVERD_BACKEND_CODEGEN_COFF_PESIGNATUREPOLICY_H
