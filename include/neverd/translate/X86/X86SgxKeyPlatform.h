//===- X86SgxKeyPlatform.h - SGX platform key operations -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Processor-specific inputs for EGETKEY and local EREPORT on an imported EPC.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86SGXKEYPLATFORM_H
#define NEVERD_TRANSLATE_X86_X86SGXKEYPLATFORM_H

#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <span>

namespace neverd::translate {

struct X86SgxPage;

/// The two extended product identifiers have implementation-specific locations
/// inside an initialized SECS. Import them by enclave identity.
struct X86SgxReportIdentity {
  std::array<uint8_t, 16> CPUSVN, ISVExtProdID, ISVFamilyID;
  std::array<uint8_t, 32> ReportKeyID;
};

/// Bind one immutable platform to the EPC bank. The platform owns the CPU
/// version relation, hidden initialized SECS fields and processor-specific key
/// hierarchy. These operations are synchronous and do not mutate guest state.
class X86SgxKeyPlatform {
public:
  virtual ~X86SgxKeyPlatform() = default;
  virtual bool acceptsCPUSVN(std::span<const uint8_t, 16> Requested) const = 0;
  virtual llvm::Expected<std::array<uint8_t, 16>>
  deriveKey(const X86SgxPage &SECS,
            std::span<const uint8_t, 512> KeyRequest) const = 0;
  virtual llvm::Expected<X86SgxReportIdentity>
  reportIdentity(const X86SgxPage &SECS) const = 0;
  /// Derive the architectural report key for TARGETINFO and KEYID, then
  /// AES-CMAC the 384-byte report body with that key.
  virtual llvm::Expected<std::array<uint8_t, 16>>
  reportMAC(const X86SgxPage &SECS, std::span<const uint8_t, 512> TargetInfo,
            std::span<const uint8_t, 32> KeyID,
            std::span<const uint8_t, 384> Body) const = 0;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86SGXKEYPLATFORM_H
