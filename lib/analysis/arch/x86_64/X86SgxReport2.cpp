//===- X86SgxReport2.cpp - Software enclave report-v2 layout --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Assemble the SGX TEEINFO and authenticate its REPORTMACSTRUCT. The bound
/// platform supplies hidden SECS fields and the CPU-specific authenticator.
//===----------------------------------------------------------------------===//

#include "X86SgxExecution.h"

#include "neverd/support/SHA512.h"
#include "neverd/translate/X86/X86SgxKeyPlatform.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::analysis::detail {
using namespace translate;

llvm::Expected<std::array<uint8_t, X86SgxExecution::Report2Size>>
X86SgxExecution::buildReport2(const X86SgxKeyPlatform &Platform,
                              const X86SgxPage &SECS,
                              std::span<const uint8_t, 64> Data,
                              X86SgxReport2MACCoverage Coverage) {
  auto Identity = Platform.reportIdentity(SECS);
  if (!Identity)
    return Identity.takeError();
  // Intel ISE 319433-063, tables 7-7 through 7-10. The SGX report type,
  // subtype, version, TEE_TCB_INFO_HASH and reserved fields are all zero.
  std::array<uint8_t, Report2Size> Result{};
  auto Info = std::span(Result).subspan<256, 512>();
  const auto &S = SECS.Bytes;
  std::copy_n(S.begin() + 20, 4, Info.begin());      // MISCSELECT
  std::copy_n(S.begin() + 256, 6, Info.begin() + 4); // product and SVNs
  std::copy_n(Identity->ISVExtProdID.begin(), 16, Info.begin() + 16);
  std::copy_n(S.begin() + 48, 48, Info.begin() + 32);   // ATTRIBUTES, MRENCLAVE
  std::copy_n(S.begin() + 128, 32, Info.begin() + 112); // MRSIGNER
  std::copy_n(S.begin() + 192, 64, Info.begin() + 176); // CONFIGID
  std::copy_n(Identity->ISVFamilyID.begin(), 16, Info.begin() + 240);
  if (llvm::support::endian::read64le(S.data() + 48) & X86SgxFeatures::SHA384) {
    auto Measurements = Platform.measurements384(SECS);
    if (!Measurements)
      return Measurements.takeError();
    std::copy_n(Measurements->MREnclave.begin(), 48, Info.begin() + 256);
    std::copy_n(Measurements->MRSigner.begin(), 48, Info.begin() + 304);
  }
  SHA512 Hash(/*Use384=*/true);
  Hash.update({reinterpret_cast<const char *>(Info.data()), Info.size()});
  const auto Digest = Hash.result();
  std::copy(Digest.begin(), Digest.end(), Result.begin() + 80);
  std::copy_n(Identity->CPUSVN.begin(), 16, Result.begin() + 16);
  std::copy(Data.begin(), Data.end(), Result.begin() + 128);
  auto MAC = Coverage == X86SgxReport2MACCoverage::Operation192
                 ? Platform.report2MAC(SECS, std::span(Result).first<192>())
                 : Platform.report2MAC224(std::span(Result).first<224>());
  if (!MAC)
    return MAC.takeError();
  std::copy(MAC->begin(), MAC->end(), Result.begin() + 224);
  return Result;
}
} // namespace neverd::analysis::detail
