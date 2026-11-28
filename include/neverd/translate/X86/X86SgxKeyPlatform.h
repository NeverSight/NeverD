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

struct X86SgxMeasurements384 {
  std::array<uint8_t, 48> MREnclave, MRSigner;
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
  /// Use the 256-bit derivation for EGETKEY256. The platform owns the hidden
  /// measurements selected by SECS.ATTRIBUTES.SHA384 and the key hierarchy.
  virtual llvm::Expected<std::array<uint8_t, 32>>
  deriveKey256(const X86SgxPage &, std::span<const uint8_t, 512>) const {
    return llvm::createStringError(
        "EGETKEY256 requires a 256-bit key provider");
  }
  /// The extended measurements have implementation-specific SECS locations.
  virtual llvm::Expected<X86SgxMeasurements384>
  measurements384(const X86SgxPage &) const {
    return llvm::createStringError(
        "EREPORT2 requires SHA-384 enclave measurements");
  }
  /// HMAC-SHA256 over REPORTMACSTRUCT[1535:0] with the platform's report
  /// MAC key, as specified by the EREPORT2 operation in Intel ISE 319433-063.
  virtual llvm::Expected<std::array<uint8_t, 32>>
  report2MAC(const X86SgxPage &, std::span<const uint8_t, 192>) const {
    return llvm::createStringError("EREPORT2 requires a platform report MAC");
  }
  /// Use the report MAC key with the full prefix consumed by EVERIFYREPORT2.
  virtual llvm::Expected<std::array<uint8_t, 32>>
  report2MAC224(std::span<const uint8_t, 224>) const {
    return llvm::createStringError(
        "EREPORT2 requires a full-prefix report MAC");
  }
  /// Authenticate all 224 bytes with the current processor's report MAC key
  /// using HMAC-SHA256. A provider error stops the instruction before
  /// retirement.
  virtual llvm::Expected<bool>
  verifyReport2MAC(std::span<const uint8_t, 224> Body,
                   std::span<const uint8_t, 32> MAC) const {
    return llvm::createStringError(
        "EVERIFYREPORT2 requires a platform report authenticator");
  }
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86SGXKEYPLATFORM_H
