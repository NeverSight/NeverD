//===- PatchFormatTestsDetail.h - Patch pipeline test harness ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The PE exception-directory mutator shared by the COFF patch
// translation units.  It is `inline` so more than one of them can be
// linked into the same binary.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_FORMAT_PATCHFORMATTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_FORMAT_PATCHFORMATTESTSDETAIL_H

#include "NeverDLiftFixture.h"

#include "neverd/object/PELayout.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/decode/Decoder.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace neverd::patch_format_test {

inline testing::AssertionResult zeroFirstExceptionRecord(const fs::path &InputPath,
                                                  const fs::path &OutputPath,
                                                  size_t RecordSize) {
  std::ifstream Input(InputPath, std::ios::binary);
  if (!Input.good())
    return testing::AssertionFailure() << "cannot read " << InputPath;
  std::vector<uint8_t> Bytes(std::istreambuf_iterator<char>(Input), {});
  auto Headers = locatePEHeaders(Bytes.data(), Bytes.size());
  if (!Headers.valid())
    return testing::AssertionFailure() << "invalid PE fixture " << InputPath;
  const auto *Directory =
      getPEDataDirectory(Headers, llvm::COFF::EXCEPTION_TABLE);
  if (!Directory || RecordSize == 0 || Directory->Size < 2 * RecordSize ||
      Directory->Size % RecordSize != 0)
    return testing::AssertionFailure()
           << "fixture needs at least two aligned runtime records";

  std::optional<size_t> TableOffset;
  forEachPESection(Headers, [&](const PESectionFields &Section, uint16_t) {
    if (TableOffset ||
        Directory->RelativeVirtualAddress < Section.VirtualAddress)
      return;
    uint64_t Delta =
        uint64_t(Directory->RelativeVirtualAddress) - Section.VirtualAddress;
    if (Delta > Section.SizeOfRawData ||
        Directory->Size > Section.SizeOfRawData - Delta)
      return;
    uint64_t Offset = uint64_t(Section.PointerToRawData) + Delta;
    if (Offset > std::numeric_limits<size_t>::max() ||
        !rangeInBounds(Offset, Directory->Size, Bytes.size()))
      return;
    TableOffset = static_cast<size_t>(Offset);
  });
  if (!TableOffset)
    return testing::AssertionFailure()
           << "exception directory is not file-backed";

  std::fill_n(Bytes.begin() + *TableOffset, RecordSize, uint8_t(0));
  std::ofstream Output(OutputPath, std::ios::binary | std::ios::trunc);
  if (!Output.good())
    return testing::AssertionFailure() << "cannot write " << OutputPath;
  Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  if (!Output.good())
    return testing::AssertionFailure()
           << "short write while creating " << OutputPath;
  return testing::AssertionSuccess();
}

} // namespace neverd::patch_format_test

#endif // NEVERD_UNITTESTS_LIFT_FORMAT_PATCHFORMATTESTSDETAIL_H
