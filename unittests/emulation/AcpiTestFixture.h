//===- AcpiTestFixture.h - Explicit synthetic ACPI SDT snapshots
//------------===//
///
/// \file
/// Explicit synthetic ACPI SDT snapshots.
///

#ifndef NEVERD_TESTS_ACPIFIXTURE_H
#define NEVERD_TESTS_ACPIFIXTURE_H
#include "neverd/emulation/DriverFirmware.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::acpifixture {
inline void checksum(std::vector<uint8_t> &B) {
  B[9] = 0;
  uint8_t Sum = 0;
  for (uint8_t C : B)
    Sum += C;
  B[9] = uint8_t(0 - Sum);
}
inline std::vector<uint8_t> table(llvm::StringRef Signature,
                                  std::vector<uint8_t> Body,
                                  uint32_t OEMRevision = 1) {
  std::vector<uint8_t> B(36, 0);
  std::copy(Signature.begin(), Signature.end(), B.begin());
  B[8] = 2;
  std::copy_n("NEVERD", 6, B.begin() + 10);
  std::copy_n("NDLAB   ", 8, B.begin() + 16);
  llvm::support::endian::write32le(B.data() + 24, OEMRevision);
  std::copy_n("ND  ", 4, B.begin() + 28);
  llvm::support::endian::write32le(B.data() + 32, 1);
  B.insert(B.end(), Body.begin(), Body.end());
  llvm::support::endian::write32le(B.data() + 4, B.size());
  checksum(B);
  return B;
}
inline DriverAcpiTables inventory() {
  // A single enabled local APIC processor record, plus two distinct SSDTs.
  // This snapshot exercises byte-query/parse semantics, not a bootable platform
  // or execution of AML/APIC hardware. The SSDT bodies contain AML Noop(s).
  return {
      {table("APIC", {0, 0, 0xe0, 0xfe, 1, 0, 0, 0, 0, 8, 0, 0, 1, 0, 0, 0}),
       table("SSDT", {0xa3}), table("SSDT", {0xa3, 0xa3}, 2)}};
}
} // namespace neverd::emulation::acpifixture
#endif
