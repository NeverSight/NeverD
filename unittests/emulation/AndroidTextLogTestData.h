//===- AndroidTextLogTestData.h - JNI failure log oracle -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Literal API 28 payloads for the generic Binder client fixture.
#ifndef NEVERD_UNITTESTS_EMULATION_ANDROIDTEXTLOGTESTDATA_H
#define NEVERD_UNITTESTS_EMULATION_ANDROIDTEXTLOGTESTDATA_H

#include "neverd/emulation/AndroidNative.h"

#include <cstdint>
#include <vector>

namespace neverd::emulation::test {
inline std::vector<NativeTextLogWrite>
binderFailureLogWrites(int32_t result = 0) {
  constexpr char failed[] =
      "\6JavaBinder\0!!! FAILED BINDER TRANSACTION !!!  (parcel size = 12)";
  constexpr char caught[] = "\6client\0caught Binder failure";
  NativeTextLogWrite failure{0, {failed, failed + sizeof(failed)}, result};
  return {failure, {0, {caught, caught + sizeof(caught)}, -11}, failure};
}
} // namespace neverd::emulation::test
#endif // NEVERD_UNITTESTS_EMULATION_ANDROIDTEXTLOGTESTDATA_H
